#define PARROT_IN_EXTENSION
#include "parrot/parrot.h"
#include "parrot/extend.h"
#include "../sixmodelobject.h"
#include "dyncall_reprs.h"
#include "CStruct.h"
#include "CArray.h"
#include "CPointer.h"

/* This representation's function pointer table. */
static MVMREPROps *this_repr;

/* Some functions we have to get references to. */
static wrap_object_t   wrap_object_func;
static create_stable_t create_stable_func;

/* How do we go from type-object to a hash value? For now, we make an integer
 * that is the address of the MVMSTable struct, which not being subject to GC will
 * never move, and is unique per type object too. */
#define CLASS_KEY(c) ((INTVAL)MVMObject_data(STABLE_PMC(c)))

/* Helper to make an introspection call, possibly with :local. */
static MVMObject * introspection_call(MVMThreadContext *tc, MVMObject *WHAT, MVMObject *HOW, STRING *name, INTVAL local) {
    MVMObject *old_ctx, *cappy;
    
    /* Look up method; if there is none hand back a null. */
    MVMObject *meth = VTABLE_find_method(tc, HOW, name);
    if (MVMObject_IS_NULL(meth))
        return meth;

    /* Set up call capture. */
    old_ctx = Parrot_pcc_get_signature(tc, CURRENT_CONTEXT(tc));
    cappy   = Parrot_pmc_new(tc, enum_class_CallContext);
    VTABLE_push_pmc(tc, cappy, HOW);
    VTABLE_push_pmc(tc, cappy, WHAT);
    if (local)
        VTABLE_set_integer_keyed_str(tc, cappy, Parrot_str_new_constant(tc, "local"), 1);

    /* Call. */
    Parrot_pcc_invoke_from_sig_object(tc, meth, cappy);

    /* Grab result. */
    cappy = Parrot_pcc_get_signature(tc, CURRENT_CONTEXT(tc));
    Parrot_pcc_set_signature(tc, CURRENT_CONTEXT(tc), old_ctx);
    return VTABLE_get_pmc_keyed_int(tc, cappy, 0);
}

/* Locates all of the attributes. Puts them onto a flattened, ordered
 * list of attributes (populating the passed flat_list). Also builds
 * the index mapping for doing named lookups. Note index is not related
 * to the storage position. */
static MVMObject * index_mapping_and_flat_list(MVMThreadContext *tc, MVMObject *mro, CStructREPRData *repr_data) {
    MVMObject    *flat_list      = Parrot_pmc_new(tc, enum_class_ResizablePMCArray);
    MVMObject    *class_list     = Parrot_pmc_new(tc, enum_class_ResizablePMCArray);
    MVMObject    *attr_map_list  = Parrot_pmc_new(tc, enum_class_ResizablePMCArray);
    STRING *name_str       = Parrot_str_new_constant(tc, "name");
    INTVAL  current_slot   = 0;
    
    INTVAL num_classes, i;
    CStructNameMap * result = NULL;
    
    /* Walk through the parents list. */
    INTVAL mro_idx = VTABLE_elements(tc, mro);
    while (mro_idx)
    {
        /* Get current class in MRO. */
        MVMObject    *type_info     = VTABLE_get_pmc_keyed_int(tc, mro, --mro_idx);
        MVMObject    *current_class = decontainerize(tc, VTABLE_get_pmc_keyed_int(tc, type_info, 0));
        
        /* Get its local parents; make sure we're not doing MI. */
        MVMObject    *parents     = VTABLE_get_pmc_keyed_int(tc, type_info, 2);
        INTVAL  num_parents = VTABLE_elements(tc, parents);
        if (num_parents <= 1) {
            /* Get attributes and iterate over them. */
            MVMObject *attributes = VTABLE_get_pmc_keyed_int(tc, type_info, 1);
            MVMObject *attr_map   = MVMObjectNULL;
            MVMObject *attr_iter  = VTABLE_get_iter(tc, attributes);
            while (VTABLE_get_bool(tc, attr_iter)) {
                /* Get attribute. */
                MVMObject * attr = VTABLE_shift_pmc(tc, attr_iter);

                /* Get its name. */
                MVMObject    *name_pmc = VTABLE_get_pmc_keyed_str(tc, attr, name_str);
                STRING *name     = VTABLE_get_string(tc, name_pmc);

                /* Allocate a slot. */
                if (MVMObject_IS_NULL(attr_map))
                    attr_map = Parrot_pmc_new(tc, enum_class_Hash);
                VTABLE_set_pmc_keyed_str(tc, attr_map, name,
                    Parrot_pmc_new_init_int(tc, enum_class_Integer, current_slot));
                current_slot++;

                /* Push attr onto the flat list. */
                VTABLE_push_pmc(tc, flat_list, attr);
            }

            /* Add to class list and map list. */
            VTABLE_push_pmc(tc, class_list, current_class);
            VTABLE_push_pmc(tc, attr_map_list, attr_map);
        }
        else {
            Parrot_ex_throw_from_c_args(tc, NULL, EXCEPTION_INVALID_OPERATION,
                "CStruct representation does not support multiple inheritance");
        }
    }

    /* We can now form the name map. */
    num_classes = VTABLE_elements(tc, class_list);
    result = (CStructNameMap *) mem_sys_allocate_zeroed(sizeof(CStructNameMap) * (1 + num_classes));
    for (i = 0; i < num_classes; i++) {
        result[i].class_key = VTABLE_get_pmc_keyed_int(tc, class_list, i);
        result[i].name_map  = VTABLE_get_pmc_keyed_int(tc, attr_map_list, i);
    }
    repr_data->name_to_index_mapping = result;

    return flat_list;
}

/* This works out an allocation strategy for the object. It takes care of
 * "inlining" storage of attributes that are natively typed, as well as
 * noting unbox targets. */
static void compute_allocation_strategy(MVMThreadContext *tc, MVMObject *repr_info, CStructREPRData *repr_data) {
    STRING *type_str = Parrot_str_new_constant(tc, "type");
    MVMObject    *flat_list;

    /*
     * We have to block GC mark here. Because "repr" is assotiated with some
     * MVMObject which is not accessible in this function. And we have to write
     * barrier this MVMObject because we are poking inside it guts directly. We
     * do have WB in caller function, but it can be triggered too late is
     * any of allocation will cause GC run.
     *
     * This is kind of minor evil until after I'll find better solution.
     */
    Parrot_block_GC_mark(tc);

    /* Compute index mapping table and get flat list of attributes. */
    flat_list = index_mapping_and_flat_list(tc, repr_info, repr_data);
    
    /* If we have no attributes in the index mapping, then just the header. */
    if (repr_data->name_to_index_mapping[0].class_key == NULL) {
        repr_data->struct_size = 1; /* avoid 0-byte malloc */
    }

    /* Otherwise, we need to compute the allocation strategy.  */
    else {
        /* We track the size of the struct, which is what we'll want offsets into. */
        INTVAL cur_size = 0;
        
        /* Get number of attributes and set up various counters. */
        INTVAL num_attrs        = VTABLE_elements(tc, flat_list);
        INTVAL info_alloc       = num_attrs == 0 ? 1 : num_attrs;
        INTVAL cur_obj_attr     = 0;
        INTVAL cur_str_attr     = 0;
        INTVAL cur_init_slot    = 0;
        INTVAL i;

        /* Allocate location/offset arrays and GC mark info arrays. */
        repr_data->num_attributes      = num_attrs;
        repr_data->attribute_locations = (INTVAL *) mem_sys_allocate(info_alloc * sizeof(INTVAL));
        repr_data->struct_offsets      = (INTVAL *) mem_sys_allocate(info_alloc * sizeof(INTVAL));
        repr_data->flattened_stables   = (MVMSTable **) mem_sys_allocate_zeroed(info_alloc * sizeof(MVMObject *));
        repr_data->member_types        = (MVMObject** )    mem_sys_allocate_zeroed(info_alloc * sizeof(MVMObject *));

        /* Go over the attributes and arrange their allocation. */
        for (i = 0; i < num_attrs; i++) {
            /* Fetch its type; see if it's some kind of unboxed type. */
            MVMObject    *attr         = VTABLE_get_pmc_keyed_int(tc, flat_list, i);
            MVMObject    *type         = VTABLE_get_pmc_keyed_str(tc, attr, type_str);
            INTVAL  type_id      = REPR(type)->ID;
            INTVAL  bits         = sizeof(void *) * 8;
            INTVAL  align        = ALIGNOF1(void *);
            if (!MVMObject_IS_NULL(type)) {
                /* See if it's a type that we know how to handle in a C struct. */
                storage_spec spec = REPR(type)->get_storage_spec(tc, STABLE(type));
                if (spec.inlineable == STORAGE_SPEC_INLINED &&
                        (spec.boxed_primitive == STORAGE_SPEC_BP_INT ||
                         spec.boxed_primitive == STORAGE_SPEC_BP_NUM)) {
                    /* It's a boxed int or num; pretty easy. It'll just live in the
                     * body of the struct. Instead of masking in i here (which
                     * would be the parallel to how we handle boxed types) we
                     * repurpose it to store the bit-width of the type, so
                     * that get_attribute_ref can find it later. */
                    bits = spec.bits;
                    align = spec.align;

                    if (bits % 8) {
                        Parrot_ex_throw_from_c_args(tc, NULL, EXCEPTION_INVALID_OPERATION,
                                "CStruct only supports native types that are a multiple of 8 bits wide (was passed: %ld)", bits);
                    }

                    repr_data->attribute_locations[i] = (bits << CSTRUCT_ATTR_SHIFT) | CSTRUCT_ATTR_IN_STRUCT;
                    repr_data->flattened_stables[i] = STABLE(type);
                    if (REPR(type)->initialize) {
                        if (!repr_data->initialize_slots)
                            repr_data->initialize_slots = (INTVAL *) mem_sys_allocate_zeroed((info_alloc + 1) * sizeof(INTVAL));
                        repr_data->initialize_slots[cur_init_slot] = i;
                        cur_init_slot++;
                    }
                }
                else if(spec.can_box & STORAGE_SPEC_CAN_BOX_STR) {
                    /* It's a string of some kind.  */
                    repr_data->num_child_objs++;
                    repr_data->attribute_locations[i] = (cur_obj_attr++ << CSTRUCT_ATTR_SHIFT) | CSTRUCT_ATTR_STRING;
                    repr_data->member_types[i] = type;
                }
                else if(type_id == get_ca_repr_id()) {
                    /* It's a CArray of some kind.  */
                    repr_data->num_child_objs++;
                    repr_data->attribute_locations[i] = (cur_obj_attr++ << CSTRUCT_ATTR_SHIFT) | CSTRUCT_ATTR_CARRAY;
                    repr_data->member_types[i] = type;
                }
                else if(type_id == get_cs_repr_id()) {
                    /* It's a CStruct. */
                    repr_data->num_child_objs++;
                    repr_data->attribute_locations[i] = (cur_obj_attr++ << CSTRUCT_ATTR_SHIFT) | CSTRUCT_ATTR_CSTRUCT;
                    repr_data->member_types[i] = type;
                }
                else if(type_id == get_cp_repr_id()) {
                    /* It's a CPointer. */
                    repr_data->num_child_objs++;
                    repr_data->attribute_locations[i] = (cur_obj_attr++ << CSTRUCT_ATTR_SHIFT) | CSTRUCT_ATTR_CPTR;
                    repr_data->member_types[i] = type;
                }
                else {
                    Parrot_ex_throw_from_c_args(tc, NULL, EXCEPTION_INVALID_OPERATION,
                        "CStruct representation only implements native int and float members so far");
                }
            }
            else {
                Parrot_ex_throw_from_c_args(tc, NULL, EXCEPTION_INVALID_OPERATION,
                    "CStruct representation requires the types of all attributes to be specified");
            }
            
            /* Do allocation. */
            /* C structure needs careful alignment. If cur_size is not aligned
             * to align bytes (cur_size % align), make sure it is before we
             * add the next element. */
            if (cur_size % align) {
                cur_size += align - cur_size % align;
            }

            repr_data->struct_offsets[i] = cur_size;
            cur_size += bits / 8;
        }

        /* Finally, put computed allocation size in place; it's body size plus
         * header size. Also number of markables and sentinels. */
        repr_data->struct_size = cur_size;
        if (repr_data->initialize_slots)
            repr_data->initialize_slots[cur_init_slot] = -1;
    }

    Parrot_unblock_GC_mark(tc);
}

/* Helper for reading an int at the specified offset. */
static INTVAL get_int_at_offset(void *data, INTVAL offset) {
    void *location = (char *)data + offset;
    return *((INTVAL *)location);
}

/* Helper for writing an int at the specified offset. */
static void set_int_at_offset(void *data, INTVAL offset, INTVAL value) {
    void *location = (char *)data + offset;
    *((INTVAL *)location) = value;
}

/* Helper for reading a num at the specified offset. */
static FLOATVAL get_num_at_offset(void *data, INTVAL offset) {
    void *location = (char *)data + offset;
    return *((FLOATVAL *)location);
}

/* Helper for writing a num at the specified offset. */
static void set_num_at_offset(void *data, INTVAL offset, FLOATVAL value) {
    void *location = (char *)data + offset;
    *((FLOATVAL *)location) = value;
}

/* Helper for reading a pointer at the specified offset. */
static void * get_ptr_at_offset(void *data, INTVAL offset) {
    void *location = (char *)data + offset;
    return *((void **)location);
}

/* Helper for writing a pointer at the specified offset. */
static void set_ptr_at_offset(void *data, INTVAL offset, void *value) {
    void *location = (char *)data + offset;
    *((void **)location) = value;
}

/* Helper for finding a slot number. */
static INTVAL try_get_slot(MVMThreadContext *tc, CStructREPRData *repr_data, MVMObject *class_key, STRING *name) {
    INTVAL slot = -1;
    if (repr_data->name_to_index_mapping) {
        CStructNameMap *cur_map_entry = repr_data->name_to_index_mapping;
        while (cur_map_entry->class_key != NULL) {
            if (cur_map_entry->class_key == class_key) {
                MVMObject *slot_pmc = VTABLE_get_pmc_keyed_str(tc, cur_map_entry->name_map, name);
                if (!MVMObject_IS_NULL(slot_pmc))
                    slot = VTABLE_get_integer(tc, slot_pmc);
                break;
            }
            cur_map_entry++;
        }
    }
    return slot;
}

/* Creates a new type object of this representation, and associates it with
 * the given HOW. */
static MVMObject * type_object_for(MVMThreadContext *tc, MVMObject *HOW) {
    /* Create new object instance. */
    CStructInstance *obj = mem_allocate_zeroed_typed(CStructInstance);

    /* Build an MVMSTable. */
    MVMObject *st_pmc = create_stable_func(tc, this_repr, HOW);
    MVMSTable *st  = STABLE_STRUCT(st_pmc);
    
    /* Create REPR data structure and hang it off the MVMSTable. */
    st->REPR_data = mem_allocate_zeroed_typed(CStructREPRData);

    /* Create type object and point it back at the MVMSTable. */
    obj->common.stable = st_pmc;
    st->WHAT = wrap_object_func(tc, obj);
    PARROT_GC_WRITE_BARRIER(tc, st_pmc);

    /* Flag it as a type object. */
    MARK_AS_TYPE_OBJECT(st->WHAT);

    return st->WHAT;
}

/* Composes the representation. */
static void compose(MVMThreadContext *tc, MVMSTable *st, MVMObject *repr_info) {
    /* Compute allocation strategy. */
    CStructREPRData *repr_data = (CStructREPRData *) st->REPR_data;
    MVMObject *attr_info = VTABLE_get_pmc_keyed_str(tc, repr_info,
            Parrot_str_new_constant(tc, "attribute"));
    compute_allocation_strategy(tc, attr_info, repr_data);
    PARROT_GC_WRITE_BARRIER(tc, st->stable_pmc);
}

/* Creates a new instance based on the type object. */
static MVMObject * allocate(MVMThreadContext *tc, MVMSTable *st) {
    CStructInstance * obj;
    CStructREPRData * repr_data = (CStructREPRData *) st->REPR_data;

    /* Allocate and set up object instance. */
    obj = (CStructInstance *) Parrot_gc_allocate_fixed_size_storage(tc, sizeof(CStructInstance));
    obj->common.stable = st->stable_pmc;
    obj->common.sc = NULL;
    obj->body.child_objs = NULL;

    /* Allocate child obj array. */
    if(repr_data->num_child_objs > 0) {
        size_t bytes = repr_data->num_child_objs*sizeof(MVMObject *);
        obj->body.child_objs = (MVMObject **) mem_sys_allocate_zeroed(bytes);
        memset(obj->body.child_objs, 0, bytes);
    }

    return wrap_object_func(tc, obj);
}

/* Initialize a new instance. */
static void initialize(MVMThreadContext *tc, MVMSTable *st, void *data) {
    CStructREPRData * repr_data = (CStructREPRData *) st->REPR_data;
    
    /* Allocate object body. */
    CStructBody *body = (CStructBody *)data;
    body->cstruct = mem_sys_allocate(repr_data->struct_size > 0 ? repr_data->struct_size : 1);
    memset(body->cstruct, 0, repr_data->struct_size);
    
    /* Initialize the slots. */
    if (repr_data->initialize_slots) {
        INTVAL i;
        for (i = 0; repr_data->initialize_slots[i] >= 0; i++) {
            INTVAL  offset = repr_data->struct_offsets[repr_data->initialize_slots[i]];
            MVMSTable *st     = repr_data->flattened_stables[repr_data->initialize_slots[i]];
            st->REPR->initialize(tc, st, (char *)body->cstruct + offset);
        }
    }
}

/* Copies to the body of one object to another. */
static void copy_to(MVMThreadContext *tc, MVMSTable *st, void *src, void *dest) {
    CStructREPRData * repr_data = (CStructREPRData *) st->REPR_data;
    CStructBody *src_body = (CStructBody *)src;
    CStructBody *dest_body = (CStructBody *)dest;
    /* XXX todo */
    /* XXX also need to shallow copy the obj array */
}

/* Helper for complaining about attribute access errors. */
PARROT_DOES_NOT_RETURN
static void no_such_attribute(MVMThreadContext *tc, const char *action, MVMObject *class_handle, STRING *name) {
    Parrot_ex_throw_from_c_args(tc, NULL, EXCEPTION_INVALID_OPERATION,
            "Can not %s non-existent attribute '%Ss' on class '%Ss'",
            action, name, VTABLE_get_string(tc, introspection_call(tc,
                class_handle, STABLE(class_handle)->HOW,
                Parrot_str_new_constant(tc, "name"), 0)));
}

/* Helper to die because this type doesn't support attributes. */
PARROT_DOES_NOT_RETURN
static void die_no_attrs(MVMThreadContext *tc) {
    Parrot_ex_throw_from_c_args(tc, NULL, EXCEPTION_INVALID_OPERATION,
            "CStruct representation attribute not yet fully implemented");
}

/* Gets the current value for an attribute. */
static MVMObject * get_attribute_boxed(MVMThreadContext *tc, MVMSTable *st, void *data, MVMObject *class_handle, STRING *name, INTVAL hint) {
    CStructREPRData *repr_data = (CStructREPRData *)st->REPR_data;
    CStructBody     *body      = (CStructBody *)data;
    INTVAL           slot;

    /* Look up slot, then offset and compute address. */
    slot = hint >= 0 ? hint :
        try_get_slot(tc, repr_data, class_handle, name);
    if (slot >= 0) {
        INTVAL type      = repr_data->attribute_locations[slot] & CSTRUCT_ATTR_MASK;
        INTVAL real_slot = repr_data->attribute_locations[slot] >> CSTRUCT_ATTR_SHIFT;

        if(type == CSTRUCT_ATTR_IN_STRUCT)
            Parrot_ex_throw_from_c_args(tc, NULL, EXCEPTION_INVALID_OPERATION,
                    "CStruct Can't perform boxed get on flattened attributes yet");
        else {
            MVMObject *obj = body->child_objs[real_slot];
            MVMObject *typeobj = repr_data->member_types[slot];

            if(!obj) {
                void *cobj = get_ptr_at_offset(body->cstruct, repr_data->struct_offsets[slot]);
                if(cobj) {
                    if(type == CSTRUCT_ATTR_CARRAY) {
                        obj = make_carray_result(tc, typeobj, cobj);
                    }
                    else if(type == CSTRUCT_ATTR_CSTRUCT) {
                        obj = make_cstruct_result(tc, typeobj, cobj);
                    }
                    else if(type == CSTRUCT_ATTR_CPTR) {
                        obj = make_cpointer_result(tc, typeobj, cobj);
                    }
                    else if(type == CSTRUCT_ATTR_STRING) {
                        char *cstr = (char *) cobj;
                        STRING *str  = Parrot_str_new_init(tc, cstr, strlen(cstr), Parrot_utf8_encoding_ptr, 0);

                        obj  = REPR(typeobj)->allocate(tc, STABLE(typeobj));
                        REPR(obj)->initialize(tc, STABLE(obj), OBJECT_BODY(obj));
                        REPR(obj)->box_funcs->set_str(tc, STABLE(obj), OBJECT_BODY(obj), str);
                        PARROT_GC_WRITE_BARRIER(tc, obj);
                    }

                    body->child_objs[real_slot] = obj;
                }
                else {
                    obj = typeobj;
                }
            }
            return obj;
        }
    }

    /* Otherwise, complain that the attribute doesn't exist. */
    no_such_attribute(tc, "get", class_handle, name);
}
static void get_attribute_native(MVMThreadContext *tc, MVMSTable *st, void *data, MVMObject *class_handle, STRING *name, INTVAL hint, NativeValue *value) {
    CStructREPRData *repr_data = (CStructREPRData *)st->REPR_data;
    CStructBody     *body      = (CStructBody *)data;
    INTVAL           slot;

    /* Look up slot, then offset and compute address. */
    slot = hint >= 0 ? hint :
        try_get_slot(tc, repr_data, class_handle, name);
    if (slot >= 0) {
        MVMSTable *st = repr_data->flattened_stables[slot];
        void *ptr = ((char *)body->cstruct) + repr_data->struct_offsets[slot];
        if (st) {
            switch (value->type) {
            case NATIVE_VALUE_INT:
                value->value.intval = st->REPR->box_funcs->get_int(tc, st, ptr);
                break;
            case NATIVE_VALUE_FLOAT:
                value->value.floatval = st->REPR->box_funcs->get_num(tc, st, ptr);
                break;
            case NATIVE_VALUE_STRING:
                value->value.stringval = st->REPR->box_funcs->get_str(tc, st, ptr);
                break;
            default:
                Parrot_ex_throw_from_c_args(tc, NULL, EXCEPTION_INVALID_OPERATION,
                    "Bad value of NativeValue.type: %d", value->type);
            }
            return;
        }
        else {
            Parrot_ex_throw_from_c_args(tc, NULL, EXCEPTION_INVALID_OPERATION,
                "Cannot read by reference from non-flattened attribute '%Ss' on class '%Ss'",
                name, VTABLE_get_string(tc, introspection_call(tc,
                    class_handle, STABLE(class_handle)->HOW,
                    Parrot_str_new_constant(tc, "name"), 0)));
        }
    }
    
    /* Otherwise, complain that the attribute doesn't exist. */
    no_such_attribute(tc, "get", class_handle, name);
}

/* Binds the given value to the specified attribute. */
static void bind_attribute_boxed(MVMThreadContext *tc, MVMSTable *st, void *data, MVMObject *class_handle, STRING *name, INTVAL hint, MVMObject *value) {
    CStructREPRData *repr_data = (CStructREPRData *)st->REPR_data;
    CStructBody     *body      = (CStructBody *)data;
    STRING          *type_str  = Parrot_str_new_constant(tc, "type");
    INTVAL            slot;

    value = decontainerize(tc, value);

    /* Try to find the slot. */
    slot = hint >= 0 ? hint :
        try_get_slot(tc, repr_data, class_handle, name);
    if (slot >= 0) {
        MVMSTable *st = repr_data->flattened_stables[slot];
        if (st)
            Parrot_ex_throw_from_c_args(tc, NULL, EXCEPTION_INVALID_OPERATION,
                    "CStruct Can't perform boxed bind on flattened attributes yet");
        else {
            INTVAL type = repr_data->attribute_locations[slot] & CSTRUCT_ATTR_MASK;
            INTVAL real_slot = repr_data->attribute_locations[slot] >> CSTRUCT_ATTR_SHIFT;

            if(IS_CONCRETE(value)) {
                void *cobj       = NULL;

                body->child_objs[real_slot] = value;

                /* Set cobj to correct pointer based on type of value. */
                if(type == CSTRUCT_ATTR_CARRAY) {
                    cobj = ((CArrayBody *) OBJECT_BODY(value))->storage;
                }
                else if(type == CSTRUCT_ATTR_CSTRUCT) {
                    cobj = ((CStructBody *) OBJECT_BODY(value))->cstruct;
                }
                else if(type == CSTRUCT_ATTR_CPTR) {
                    cobj = ((CPointerBody *) OBJECT_BODY(value))->ptr;
                }
                else if(type == CSTRUCT_ATTR_STRING) {
                    STRING *str  = REPR(value)->box_funcs->get_str(tc, STABLE(value), OBJECT_BODY(value));
                    cobj = Parrot_str_to_encoded_cstring(tc, str, Parrot_utf8_encoding_ptr);
                }

                set_ptr_at_offset(body->cstruct, repr_data->struct_offsets[slot], cobj);
            }
            else {
                body->child_objs[real_slot] = NULL;
                set_ptr_at_offset(body->cstruct, repr_data->struct_offsets[slot], NULL);
            }
        }
    }
    else {
        /* Otherwise, complain that the attribute doesn't exist. */
        no_such_attribute(tc, "bind", class_handle, name);
    }
}
static void bind_attribute_native(MVMThreadContext *tc, MVMSTable *st, void *data, MVMObject *class_handle, STRING *name, INTVAL hint, NativeValue *value) {
    CStructREPRData *repr_data = (CStructREPRData *)st->REPR_data;
    CStructBody     *body      = (CStructBody *)data;
    INTVAL            slot;

    /* Try to find the slot. */
    slot = hint >= 0 ? hint :
        try_get_slot(tc, repr_data, class_handle, name);
    if (slot >= 0) {
        MVMSTable *st = repr_data->flattened_stables[slot];
        if (st) {
            void *ptr = ((char *)body->cstruct) + repr_data->struct_offsets[slot];
            switch (value->type) {
            case NATIVE_VALUE_INT:
                st->REPR->box_funcs->set_int(tc, st, ptr, value->value.intval);
                break;
            case NATIVE_VALUE_FLOAT:
                st->REPR->box_funcs->set_num(tc, st, ptr, value->value.floatval);
                break;
            case NATIVE_VALUE_STRING:
                st->REPR->box_funcs->set_str(tc, st, ptr, value->value.stringval);
                break;
            default:
                Parrot_ex_throw_from_c_args(tc, NULL, EXCEPTION_INVALID_OPERATION,
                    "Bad value of NativeValue.type: %d", value->type);
            }
            return;
        }
        else
            Parrot_ex_throw_from_c_args(tc, NULL, EXCEPTION_INVALID_OPERATION,
                "Can not bind by reference to non-flattened attribute '%Ss' on class '%Ss'",
                name, VTABLE_get_string(tc, introspection_call(tc,
                    class_handle, STABLE(class_handle)->HOW,
                    Parrot_str_new_constant(tc, "name"), 0)));
    }
    else {
        /* Otherwise, complain that the attribute doesn't exist. */
        no_such_attribute(tc, "bind", class_handle, name);
    }
}

/* Checks if an attribute has been initialized. */
static INTVAL is_attribute_initialized(MVMThreadContext *tc, MVMSTable *st, void *data, MVMObject *ClassHandle, STRING *Name, INTVAL Hint) {
    die_no_attrs(tc);
}

/* Gets the hint for the given attribute ID. */
static INTVAL hint_for(MVMThreadContext *tc, MVMSTable *st, MVMObject *class_handle, STRING *name) {
    return NO_HINT;
}

/* This Parrot-specific addition to the API is used to mark an object. */
static void gc_mark(MVMThreadContext *tc, MVMSTable *st, void *data) {
    CStructREPRData *repr_data = (CStructREPRData *) st->REPR_data;
    CStructBody *body = (CStructBody *)data;
    INTVAL i;
    for (i = 0; i < repr_data->num_child_objs; i++)
        Parrot_gc_mark_PMC_alive(tc, body->child_objs[i]);
}

static void gc_mark_repr_data(MVMThreadContext *tc, MVMSTable *st) {
    CStructREPRData *repr_data = (CStructREPRData *) st->REPR_data;
    CStructNameMap *map = repr_data->name_to_index_mapping;
    INTVAL i;

    if (!map) return;

    for (i = 0; map[i].class_key; i++) {
        Parrot_gc_mark_PMC_alive(tc, map[i].class_key);
        Parrot_gc_mark_PMC_alive(tc, map[i].name_map);
    }
}

/* This is called to do any cleanup of resources when an object gets
 * embedded inside another one. Never called on a top-level object. */
static void gc_cleanup(MVMThreadContext *tc, MVMSTable *st, void *data) {
    CStructBody *body = (CStructBody *)data;
    if (body->child_objs)
        mem_sys_free(body->child_objs);
    if (body->cstruct)
        mem_sys_free(body->cstruct);
}

/* This Parrot-specific addition to the API is used to free an object. */
static void gc_free(MVMThreadContext *tc, MVMObject *obj) {
    CStructREPRData *repr_data = (CStructREPRData *)STABLE(obj)->REPR_data;
	gc_cleanup(tc, STABLE(obj), OBJECT_BODY(obj));
    if (IS_CONCRETE(obj))
		Parrot_gc_free_fixed_size_storage(tc, sizeof(CStructInstance), MVMObject_data(obj));
	else
		mem_sys_free(MVMObject_data(obj));
    MVMObject_data(obj) = NULL;
}

/* Gets the storage specification for this representation. */
static storage_spec get_storage_spec(MVMThreadContext *tc, MVMSTable *st) {
    storage_spec spec;
    spec.inlineable = STORAGE_SPEC_REFERENCE;
    spec.boxed_primitive = STORAGE_SPEC_BP_NONE;
    spec.can_box = 0;
    spec.bits = sizeof(void *) * 8;
    spec.align = ALIGNOF1(void *);
    return spec;
}

/* Serializes the REPR data. */
static void serialize_repr_data(MVMThreadContext *tc, MVMSTable *st, SerializationWriter *writer) {
    CStructREPRData *repr_data = (CStructREPRData *)st->REPR_data;
    /* Could do this, but can also re-compute it each time for now. */
}

/* Deserializes the REPR data. */
static void deserialize_repr_data(MVMThreadContext *tc, MVMSTable *st, SerializationReader *reader) {
    /* Just allocating it will do for now. */
    st->REPR_data = mem_sys_allocate_zeroed(sizeof(CStructREPRData));
}

/* Initializes the CStruct representation. */
MVMREPROps * CStruct_initialize(MVMThreadContext *tc,
        wrap_object_t wrap_object_func_ptr,
        create_stable_t create_stable_func_ptr) {
    /* Stash away functions passed wrapping functions. */
    wrap_object_func = wrap_object_func_ptr;
    create_stable_func = create_stable_func_ptr;

    /* Allocate and populate the representation function table. */
    this_repr = mem_allocate_zeroed_typed(MVMREPROps);
    this_repr->type_object_for = type_object_for;
    this_repr->compose = compose;
    this_repr->allocate = allocate;
    this_repr->initialize = initialize;
    this_repr->copy_to = copy_to;
    this_repr->attr_funcs = mem_allocate_typed(MVMREPROps_Attribute);
    this_repr->attr_funcs->get_attribute_boxed = get_attribute_boxed;
    this_repr->attr_funcs->get_attribute_native = get_attribute_native;
    this_repr->attr_funcs->bind_attribute_boxed = bind_attribute_boxed;
    this_repr->attr_funcs->bind_attribute_native = bind_attribute_native;
    this_repr->attr_funcs->is_attribute_initialized = is_attribute_initialized;
    this_repr->attr_funcs->hint_for = hint_for;
    this_repr->gc_mark = gc_mark;
    this_repr->gc_free = gc_free;
    this_repr->gc_mark_repr_data = gc_mark_repr_data;
    this_repr->gc_cleanup = gc_cleanup;
    this_repr->get_storage_spec = get_storage_spec;
    this_repr->serialize_repr_data = serialize_repr_data;
    this_repr->deserialize_repr_data = deserialize_repr_data;
    return this_repr;
}
