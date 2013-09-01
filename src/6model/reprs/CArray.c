#include <moarvm.h>
#include <core/nativecall.h>
#include "CArray.h"
#include "CStruct.h"
#include "CPointer.h"

#define ALIGNOF(type) \
    ((MVMuint16)offsetof(struct { char dummy; type member; }, member))

typedef MVMObject * (* wrap_object_t) (MVMThreadContext *tc, void *obj);
typedef MVMObject * (* create_stable_t) (MVMThreadContext *tc, MVMREPROps *REPR, MVMObject *HOW);

/* This representation's function pointer table. */
static MVMREPROps *this_repr;

/* Some functions we have to get references to. */
static wrap_object_t   wrap_object_func;
static create_stable_t create_stable_func;

/* Gets size and type information to put it into the REPR data. */
static void fill_repr_data(MVMThreadContext *tc, MVMSTable *st) {
#if 0
    MVMCArrayREPRData *repr_data = (MVMCArrayREPRData *)st->REPR_data;
    MVMObject *old_ctx, *cappy;
    MVMStorageSpec ss;
    MVMuint16 type_id;

    /* Look up "of" method. */
    MVMObject *meth = MVM_6model_find_method(tc, st->WHAT,
        Parrot_str_new_constant(tc, "of"));
    if (!meth)
        MVM_exception_throw_adhoc(tc, "CArray representation expects an 'of' method, specifying the element type");

    /* Call it to get the type. */
    old_ctx = Parrot_pcc_get_signature(tc, CURRENT_CONTEXT(tc));
    cappy   = Parrot_pmc_new(tc, enum_class_CallContext);
    VTABLE_push_pmc(tc, cappy, st->WHAT);
    Parrot_pcc_invoke_from_sig_object(tc, meth, cappy);
    cappy = Parrot_pcc_get_signature(tc, CURRENT_CONTEXT(tc));
    Parrot_pcc_set_signature(tc, CURRENT_CONTEXT(tc), old_ctx);
    repr_data->elem_type = VTABLE_get_pmc_keyed_int(tc, cappy, 0);
    
    /* Ensure we got a type. */
    if (MVMObject_IS_NULL(repr_data->elem_type))
        MVM_exception_throw_adhoc(tc, "CArray representation expects an 'of' method, specifying the element type");

    /* What we do next depends on what kind of type we have. */
    ss = REPR(repr_data->elem_type)->get_storage_spec(tc, STABLE(repr_data->elem_type));
    type_id = REPR(repr_data->elem_type)->ID;
    if (ss.boxed_primitive == MVM_STORAGE_SPEC_BP_INT) {
        if (ss.bits == 8 || ss.bits == 16 || ss.bits == 32 || ss.bits == 64)
            repr_data->elem_size = ss.bits / 8;
        else
            MVM_exception_throw_adhoc(tc, "CArray representation can only have 8, 16, 32 or 64 bit integer elements");
        repr_data->elem_kind = MVM_CARRAY_ELEM_KIND_NUMERIC;
    }
    else if (ss.boxed_primitive == MVM_STORAGE_SPEC_BP_NUM) {
        if (ss.bits == 32 || ss.bits == 64)
            repr_data->elem_size = ss.bits / 8;
        else
            MVM_exception_throw_adhoc(tc, "CArray representation can only have 32 or 64 bit floating point elements");
        repr_data->elem_kind = MVM_CARRAY_ELEM_KIND_NUMERIC;
    }
    else if (ss.can_box & MVM_STORAGE_SPEC_CAN_BOX_STR) {
        repr_data->elem_size = sizeof(MVMObject *);
        repr_data->elem_kind = MVM_CARRAY_ELEM_KIND_STRING;
    }
    else if (type_id == get_ca_repr_id()) {
        repr_data->elem_kind = MVM_CARRAY_ELEM_KIND_CARRAY;
        repr_data->elem_size = sizeof(void *);
    }
    else if (type_id == get_cs_repr_id()) {
        repr_data->elem_kind = MVM_CARRAY_ELEM_KIND_CSTRUCT;
        repr_data->elem_size = sizeof(void *);
    }
    else if (type_id == get_cp_repr_id()) {
        repr_data->elem_kind = MVM_CARRAY_ELEM_KIND_CPOINTER;
        repr_data->elem_size = sizeof(void *);
    }
    else {
        MVM_exception_throw_adhoc(tc, "CArray may only contain native integers and numbers, strings, C Structs or C Pointers");
    }
#endif
}

/* Creates a new type object of this representation, and associates it with
 * the given HOW. */
static MVMObject * type_object_for(MVMThreadContext *tc, MVMObject *HOW) {
    MVMSTable *st = MVM_gc_allocate_stable(tc, this_repr, HOW);

    MVMROOT(tc, st, {
        MVMObject *obj = MVM_gc_allocate_type_object(tc, st);

        MVM_ASSIGN_REF(tc, st, st->WHAT, obj);
        st->size = sizeof(MVMCArray);

        /* Create REPR data structure and hand it off the MVMSTable. */
        st->REPR_data = calloc(1, sizeof(MVMCArrayREPRData));

        /* FIXME??? */
        st->WHAT = wrap_object_func(tc, obj);
    });

    return st->WHAT;
}

/* Composes the representation. */
static void compose(MVMThreadContext *tc, MVMSTable *st, MVMObject *repr_info) {
    /* TODO */
}

/* Creates a new instance based on the type object. */
static MVMObject * allocate(MVMThreadContext *tc, MVMSTable *st) {
    MVMCArray *obj = calloc(1, sizeof(MVMCArray));
    MVMCArrayREPRData *repr_data = (MVMCArrayREPRData *)st->REPR_data;
    obj->common.st = st;
    if (!repr_data->elem_size)
        fill_repr_data(tc, st);
    return wrap_object_func(tc, obj);
}

/* Initialize a new instance. */
static void initialize(MVMThreadContext *tc, MVMSTable *st, void *data) {
    /* If we're initialized, presumably we're going to be
     * managing the memory in this array ourself. */
    MVMCArrayREPRData *repr_data = (MVMCArrayREPRData *)st->REPR_data;
    MVMCArrayBody *body = (MVMCArrayBody *)data;
    body->storage = malloc(4 * repr_data->elem_size);
    body->managed = 1;
    /* Don't need child_objs for numerics or strings. */
    if (repr_data->elem_kind == MVM_CARRAY_ELEM_KIND_NUMERIC)
        body->child_objs = NULL;
    else
        body->child_objs = calloc(4, sizeof(MVMObject *));
    body->allocated = 4;
    body->elems = 0;
}

/* Copies to the body of one object to another. */
static void copy_to(MVMThreadContext *tc, MVMSTable *st, void *src, void *dest) {
    MVMCArrayREPRData *repr_data = (MVMCArrayREPRData *)st->REPR_data;
    MVMCArrayBody     *src_body  = (MVMCArrayBody *)src;
    MVMCArrayBody     *dest_body = (MVMCArrayBody *)dest;
    if (src_body->managed) {
        MVMuint64 alsize = src_body->allocated * repr_data->elem_size;
        dest_body->storage = malloc(alsize);
        memcpy(dest_body->storage, src_body->storage, alsize);
    }
    else {
        dest_body->storage = src_body->storage;
    }
    dest_body->managed = src_body->managed;
    dest_body->allocated = src_body->allocated;
    dest_body->elems = src_body->elems;
}

/* This is called to do any cleanup of resources when an object gets
 * embedded inside another one. Never called on a top-level object. */
static void gc_cleanup(MVMThreadContext *tc, MVMSTable *st, void *data) {
    MVMCArrayBody *body = (MVMCArrayBody *)data;
    if (body->managed) {
        free(body->storage);
        if (body->child_objs)
            free(body->child_objs);
    }
}

/* This Parrot-specific addition to the API is used to free an object. */
static void gc_free(MVMThreadContext *tc, MVMObject *obj) {
    gc_cleanup(tc, STABLE(obj), OBJECT_BODY(obj));
    free(MVMObject_data(obj));
    MVMObject_data(obj) = NULL;
}

static void gc_mark(MVMThreadContext *tc, MVMSTable *st, void *data) {
    MVMCArrayREPRData *repr_data = (MVMCArrayREPRData *) st->REPR_data;
    MVMCArrayBody *body = (MVMCArrayBody *)data;
    MVMuint64 i;

    /* Don't traverse child_objs list if there isn't one. */
    if (!body->child_objs) return;

    for (i = 0; i < body->elems; i++)
        if (body->child_objs[i])
            Parrot_gc_mark_PMC_alive(tc, body->child_objs[i]);
}

/* Gets the storage specification for this representation. */
static MVMStorageSpec get_storage_spec(MVMThreadContext *tc, MVMSTable *st) {
    MVMStorageSpec spec;
    spec.inlineable = MVM_STORAGE_SPEC_REFERENCE;
    spec.boxed_primitive = MVM_STORAGE_SPEC_BP_NONE;
    spec.can_box = 0;
    spec.bits = sizeof(void *) * 8;
    spec.align = ALIGNOF(void *);
    return spec;
}

MVM_NO_RETURN
static void die_pos_nyi(MVMThreadContext *tc) {
    MVM_exception_throw_adhoc(tc, "CArray representation does not fully positional storage yet");
}
static void expand(MVMThreadContext *tc, MVMCArrayREPRData *repr_data, MVMCArrayBody *body, INTVAL min_size) {
    int is_complex = 0;
    MVMuint64 next_size = body->allocated ? 2 * body->allocated : 4;
    if (min_size > next_size)
        next_size = min_size;
    if (body->managed)
        body->storage = realloc(body->storage, next_size * repr_data->elem_size);

    is_complex = (repr_data->elem_kind == MVM_CARRAY_ELEM_KIND_CARRAY
               || repr_data->elem_kind == MVM_CARRAY_ELEM_KIND_CPOINTER
               || repr_data->elem_kind == MVM_CARRAY_ELEM_KIND_CSTRUCT
               || repr_data->elem_kind == MVM_CARRAY_ELEM_KIND_STRING);
    if (is_complex) {
        MVMuint64 bytes = body->allocated * sizeof(MVMObject *);
        MVMuint64 next_bytes = next_size * sizeof(MVMObject *);
        body->child_objs = realloc(body->child_objs, next_bytes);
        memset((char *)body->child_objs + bytes, 0, next_bytes - bytes);
    }
    body->allocated = next_size;
}
static void at_pos_native(MVMThreadContext *tc, MVMSTable *st, void *data, INTVAL index, NativeValue *value) {
    MVMCArrayREPRData *repr_data = (MVMCArrayREPRData *)st->REPR_data;
    MVMCArrayBody     *body      = (MVMCArrayBody *)data;
    MVMSTable         *type_st   = STABLE(repr_data->elem_type);
    void           *ptr       = ((char *)body->storage) + index * repr_data->elem_size;
    if (body->managed && index >= body->elems) {
        switch (value->type) {
        case NATIVE_VALUE_INT:
            value->value.intval = 0;
            return;
        case NATIVE_VALUE_FLOAT: {
            double x = 0.0;
            value->value.floatval = 0.0/x;
            return;
        }
        case NATIVE_VALUE_STRING:
            value->value.stringval = STRINGNULL;
            return;
        default:
            MVM_exception_throw_adhoc(tc, "Bad value of NativeValue.type: %d", value->type);
        }
    }
    switch (repr_data->elem_kind) {
        case MVM_CARRAY_ELEM_KIND_NUMERIC:
            switch (value->type) {
            case NATIVE_VALUE_INT:
                value->value.intval = type_st->REPR->box_funcs->get_int(tc, type_st, ptr);
                break;
            case NATIVE_VALUE_FLOAT:
                value->value.floatval = type_st->REPR->box_funcs->get_num(tc, type_st, ptr);
                break;
            case NATIVE_VALUE_STRING:
                value->value.stringval = type_st->REPR->box_funcs->get_str(tc, type_st, ptr);
                break;
            default:
                MVM_exception_throw_adhoc(tc, "Bad value of NativeValue.type: %d", value->type);
            }
            break;
        default:
            MVM_exception_throw_adhoc(tc, "at_pos_native on CArray REPR only usable with numeric element types");
    }
}
static MVMObject * make_object(MVMThreadContext *tc, MVMSTable *st, void *data) {
    MVMCArrayREPRData *repr_data = (MVMCArrayREPRData *)st->REPR_data;
    MVMCArrayBody     *body      = (MVMCArrayBody *)data;
    MVMObject            *retval;

    switch (repr_data->elem_kind) {
        case MVM_CARRAY_ELEM_KIND_STRING:
        {
            char   *elem = (char *) data;
            MVMString *str  = Parrot_str_new_init(tc, elem, strlen(elem), Parrot_utf8_encoding_ptr, 0);
            MVMObject    *obj  = REPR(repr_data->elem_type)->allocate(tc, STABLE(repr_data->elem_type));
            REPR(obj)->initialize(tc, STABLE(obj), OBJECT_BODY(obj));
            REPR(obj)->box_funcs->set_str(tc, STABLE(obj), OBJECT_BODY(obj), str);
            PARROT_GC_WRITE_BARRIER(tc, obj);
            retval = obj;
            break;
        }
        case MVM_CARRAY_ELEM_KIND_CARRAY:
            retval = make_carray_result(tc, repr_data->elem_type, data);
            break;
        case MVM_CARRAY_ELEM_KIND_CPOINTER:
            retval = make_cpointer_result(tc, repr_data->elem_type, data);
            break;
        case MVM_CARRAY_ELEM_KIND_CSTRUCT:
            retval = make_cstruct_result(tc, repr_data->elem_type, data);
            break;
        default:
            MVM_exception_throw_adhoc(tc, "Fatal error: unknown CArray elem_kind (%d) in make_object", repr_data->elem_kind);
    }

    return retval;
}
static MVMObject * at_pos_boxed(MVMThreadContext *tc, MVMSTable *st, void *data, INTVAL index) {
    MVMCArrayREPRData *repr_data = (MVMCArrayREPRData *)st->REPR_data;
    MVMCArrayBody     *body      = (MVMCArrayBody *)data;
    void **storage            = (void **) body->storage;
    MVMObject *obj;

    switch (repr_data->elem_kind) {
        case MVM_CARRAY_ELEM_KIND_STRING:
        case MVM_CARRAY_ELEM_KIND_CARRAY:
        case MVM_CARRAY_ELEM_KIND_CPOINTER:
        case MVM_CARRAY_ELEM_KIND_CSTRUCT:
            break;
        default:
            MVM_exception_throw_adhoc(tc, "at_pos_boxed on CArray REPR not usable with this element type");
    }

    if (body->managed && index >= body->elems)
        return repr_data->elem_type;

    if (body->managed) {
        /* We manage this array. */
        if (index < body->elems && body->child_objs[index])
            return body->child_objs[index];
        else if (index < body->elems) {
            /* Someone's changed the array since the cached object was
             * created. Recreate it. */
            obj = make_object(tc, st, storage[index]);
            body->child_objs[index] = obj;
            return obj;
        }
        else
            return repr_data->elem_type;
    }
    else {
        /* Array comes from C. */
        /* Enlarge child_objs if needed. */
        if (index >= body->allocated)
            expand(tc, repr_data, body, index+1);
        if (index >= body->elems)
            body->elems = index + 1;

        /* We've already fetched this object. Return that. */
        if (storage[index] && body->child_objs[index]) {
            return body->child_objs[index];
        }
        /* No cached object, but non-NULL pointer in array. Construct object,
         * put it in the cache and return it. */
        else if (storage[index]) {
            obj = make_object(tc, st, storage[index]);
            body->child_objs[index] = obj;
            return obj;
        }
        /* NULL pointer in the array, just return the type object. */
        else {
            return repr_data->elem_type;
        }
    }
}
static void bind_pos_native(MVMThreadContext *tc, MVMSTable *st, void *data, INTVAL index, NativeValue *value) {
    MVMCArrayREPRData *repr_data = (MVMCArrayREPRData *)st->REPR_data;
    MVMCArrayBody     *body      = (MVMCArrayBody *)data;
    MVMSTable         *type_st   = STABLE(repr_data->elem_type);
    void           *ptr       = ((char *)body->storage) + index * repr_data->elem_size;
    if (body->managed && index >= body->allocated)
        expand(tc, repr_data, body, index + 1);
    if (index >= body->elems)
        body->elems = index + 1;
    switch (repr_data->elem_kind) {
        case MVM_CARRAY_ELEM_KIND_NUMERIC:
            switch (value->type) {
            case NATIVE_VALUE_INT:
                type_st->REPR->box_funcs->set_int(tc, type_st, ptr, value->value.intval);
                break;
            case NATIVE_VALUE_FLOAT:
                type_st->REPR->box_funcs->set_num(tc, type_st, ptr, value->value.floatval);
                break;
            case NATIVE_VALUE_STRING:
                type_st->REPR->box_funcs->set_str(tc, type_st, ptr, value->value.stringval);
                break;
            default:
                MVM_exception_throw_adhoc(tc, "Bad value of NativeValue.type: %d", value->type);
            }
            break;
        default:
            MVM_exception_throw_adhoc(tc, "bind_pos_native on CArray REPR only usable with numeric element types");
    }
}
static void bind_pos_boxed(MVMThreadContext *tc, MVMSTable *st, void *data, INTVAL index, MVMObject *obj) {
    MVMCArrayREPRData *repr_data = (MVMCArrayREPRData *)st->REPR_data;
    MVMCArrayBody     *body      = (MVMCArrayBody *)data;
    void **storage = (void **) body->storage;
    void *cptr; /* Pointer to C data. */

    /* Enlarge child_objs if needed. */
    if (index >= body->allocated)
        expand(tc, repr_data, body, index+1);
    if (index >= body->elems)
        body->elems = index + 1;

    /* Make sure the type isn't something we can't handle. */
    switch (repr_data->elem_kind) {
        case MVM_CARRAY_ELEM_KIND_STRING:
        case MVM_CARRAY_ELEM_KIND_CARRAY:
        case MVM_CARRAY_ELEM_KIND_CSTRUCT:
        case MVM_CARRAY_ELEM_KIND_CPOINTER:
            break;
        default:
            MVM_exception_throw_adhoc(tc, "bind_pos_boxed on CArray REPR not usable with this element type");
    }

    if (IS_CONCRETE(obj)) {
        switch (repr_data->elem_kind) {
            case MVM_CARRAY_ELEM_KIND_STRING:
            {
                MVMString *str  = REPR(obj)->box_funcs->get_str(tc, STABLE(obj), OBJECT_BODY(obj));
                cptr = Parrot_str_to_encoded_cstring(tc, str, Parrot_utf8_encoding_ptr);
                break;
            }
            case MVM_CARRAY_ELEM_KIND_CARRAY:
                cptr = ((MVMCArrayBody *) OBJECT_BODY(obj))->storage;
                break;
            case MVM_CARRAY_ELEM_KIND_CSTRUCT:
                cptr = ((CStructBody *) OBJECT_BODY(obj))->cstruct;
                break;
            case MVM_CARRAY_ELEM_KIND_CPOINTER:
                cptr = ((MVMCPointerBody *) OBJECT_BODY(obj))->ptr;
                break;
            default:
                MVM_exception_throw_adhoc(tc, "Fatal error: unknown CArray elem_kind (%d) in bind_pos_boxed", repr_data->elem_kind);
        }
    }
    else {
        cptr = NULL;
    }

    body->child_objs[index] = obj;
    storage[index] = cptr;
}
static MVMSTable * get_elem_stable(MVMThreadContext *tc, MVMSTable *st) {
    MVMCArrayREPRData *repr_data = (MVMCArrayREPRData *)st->REPR_data;
    return STABLE(repr_data->elem_type);
}
static void push_boxed(MVMThreadContext *tc, MVMSTable *st, void *data, MVMObject *obj) {
    die_pos_nyi(tc);
}
static MVMObject * pop_boxed(MVMThreadContext *tc, MVMSTable *st, void *data) {
    die_pos_nyi(tc);
}
static void unshift_boxed(MVMThreadContext *tc, MVMSTable *st, void *data, MVMObject *obj) {
    die_pos_nyi(tc);
}
static MVMObject * shift_boxed(MVMThreadContext *tc, MVMSTable *st, void *data) {
    die_pos_nyi(tc);
}
static MVMuint64 elems(MVMThreadContext *tc, MVMSTable *st, void *data) {
    MVMCArrayBody     *body      = (MVMCArrayBody *)data;
    if (body->managed)
        return body->elems;
    MVM_exception_throw_adhoc(tc, "Don't know how many elements a C array returned from a library has");
}

/* Serializes the REPR data. */
static void serialize_repr_data(MVMThreadContext *tc, MVMSTable *st, MVMSerializationWriter *writer) {
    MVMCArrayREPRData *repr_data = (MVMCArrayREPRData *)st->REPR_data;
    writer->write_int(tc, writer, repr_data->elem_size);
    writer->write_ref(tc, writer, repr_data->elem_type);
    writer->write_int(tc, writer, repr_data->elem_kind);
}

/* Deserializes the REPR data. */
static void deserialize_repr_data(MVMThreadContext *tc, MVMSTable *st, MVMSerializationReader *reader) {
    MVMCArrayREPRData *repr_data = (MVMCArrayREPRData *) mem_sys_allocate_zeroed(sizeof(MVMCArrayREPRData));
    st->REPR_data = (MVMCArrayREPRData *) repr_data;
    repr_data->elem_size = reader->read_int(tc, reader);
    repr_data->elem_type = reader->read_ref(tc, reader);
    repr_data->elem_kind = reader->read_int(tc, reader);
}

/* Initializes the CArray representation. */
MVMREPROps * MVMCArray_initialize(MVMThreadContext *tc,
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
    this_repr->gc_cleanup = gc_cleanup;
    this_repr->gc_free = gc_free;
    this_repr->gc_mark = gc_mark;
    this_repr->get_storage_spec = get_storage_spec;
    this_repr->pos_funcs = mem_allocate_zeroed_typed(MVMREPROps_Positional);
    this_repr->pos_funcs->at_pos_native = at_pos_native;
    this_repr->pos_funcs->at_pos_boxed = at_pos_boxed;
    this_repr->pos_funcs->bind_pos_native = bind_pos_native;
    this_repr->pos_funcs->bind_pos_boxed = bind_pos_boxed;
    this_repr->pos_funcs->push_boxed = push_boxed;
    this_repr->pos_funcs->pop_boxed = pop_boxed;
    this_repr->pos_funcs->unshift_boxed = unshift_boxed;
    this_repr->pos_funcs->shift_boxed = shift_boxed;
    this_repr->pos_funcs->get_elem_stable = get_elem_stable;
    this_repr->elems = elems;
    this_repr->serialize_repr_data = serialize_repr_data;
    this_repr->deserialize_repr_data = deserialize_repr_data;
    
    return this_repr;
}
