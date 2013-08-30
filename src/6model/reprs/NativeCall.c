#include <moarvm.h>
#include "NativeCall.h"

/* This representation's function pointer table. */
static MVMREPROps *this_repr;

/* Some functions we have to get references to. */
static wrap_object_t   wrap_object_func;
static create_stable_t create_stable_func;

/* Creates a new type object of this representation, and associates it with
 * the given HOW. */
static MVMObject * type_object_for(MVMThreadContext *tc, MVMObject *HOW) {
    /* Create new object instance. */
    NativeCallInstance *obj = mem_allocate_zeroed_typed(NativeCallInstance);

    /* Build an MVMSTable. */
    MVMObject *st_pmc = create_stable_func(tc, this_repr, HOW);
    MVMSTable *st  = STABLE_STRUCT(st_pmc);

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
    /* Nothing to do. */
}

/* Creates a new instance based on the type object. */
static MVMObject * allocate(MVMThreadContext *tc, MVMSTable *st) {
    NativeCallInstance *obj = mem_allocate_zeroed_typed(NativeCallInstance);
    obj->common.stable = st->stable_pmc;
    return wrap_object_func(tc, obj);
}

/* Initialize a new instance. */
static void initialize(MVMThreadContext *tc, MVMSTable *st, void *data) {
    /* Nothing to do here. */
}

/* Copies to the body of one object to another. */
static void copy_to(MVMThreadContext *tc, MVMSTable *st, void *src, void *dest) {
    NativeCallBody *src_body = (NativeCallBody *)src;
    NativeCallBody *dest_body = (NativeCallBody *)dest;
    
    /* Need a fresh handle for resource management purposes. */
    if (src_body->lib_name) {
        dest_body->lib_name = (char *) mem_sys_allocate(strlen(src_body->lib_name) + 1);
        strcpy(dest_body->lib_name, src_body->lib_name);
        dest_body->lib_handle = dlLoadLibrary(dest_body->lib_name);
    }
    
    /* Rest is just simple copying. */
    dest_body->entry_point = src_body->entry_point;
    dest_body->convention = src_body->convention;
    dest_body->num_args = src_body->num_args;
    if (src_body->arg_types) {
        dest_body->arg_types = (INTVAL *) mem_sys_allocate(sizeof(INTVAL) * (src_body->num_args ? src_body->num_args : 1));
        memcpy(dest_body->arg_types, src_body->arg_types, src_body->num_args * sizeof(INTVAL));
    }
    dest_body->ret_type = src_body->ret_type;
}

/* This is called to do any cleanup of resources when an object gets
 * embedded inside another one. Never called on a top-level object. */
static void gc_cleanup(MVMThreadContext *tc, MVMSTable *st, void *data) {
    NativeCallBody *body = (NativeCallBody *)data;
    if (body->lib_name)
        Parrot_str_free_cstring(body->lib_name);
    if (body->lib_handle)
        dlFreeLibrary(body->lib_handle);
    if (body->arg_types)
        mem_sys_free(body->arg_types);
    if (body->arg_info)
        mem_sys_free(body->arg_info);
}

/* This Parrot-specific addition to the API is used to free an object. */
static void gc_free(MVMThreadContext *tc, MVMObject *obj) {
    gc_cleanup(tc, STABLE(obj), OBJECT_BODY(obj));
    mem_sys_free(MVMObject_data(obj));
    MVMObject_data(obj) = NULL;
}

static void gc_mark(MVMThreadContext *tc, MVMSTable *st, void *data) {
    NativeCallBody *body = (NativeCallBody *)data;

    if (body->arg_info) {
        INTVAL i;
        for (i = 0; i < body->num_args; i++) {
            if (body->arg_info[i])
                Parrot_gc_mark_PMC_alive(tc, body->arg_info[i]);
        }
    }
}

/* Gets the storage specification for this representation. */
static storage_spec get_storage_spec(MVMThreadContext *tc, MVMSTable *st) {
    storage_spec spec;
    spec.inlineable = STORAGE_SPEC_INLINED;
    spec.bits = sizeof(NativeCallBody) * 8;
    spec.align = ALIGNOF1(void *);
    spec.boxed_primitive = STORAGE_SPEC_BP_NONE;
    spec.can_box = 0;
    return spec;
}

/* We can't actually serialize the handle, but since this REPR gets inlined
 * we just do nothing here since it may well have never been opened. Various
 * more involved approaches are possible... */
static void serialize(MVMThreadContext *tc, MVMSTable *st, void *data, SerializationWriter *writer) {
}
static void deserialize(MVMThreadContext *tc, MVMSTable *st, void *data, SerializationReader *reader) {
}

/* Initializes the NativeCall representation. */
MVMREPROps * NativeCall_initialize(MVMThreadContext *tc,
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
    this_repr->gc_mark = gc_mark;
    this_repr->gc_free = gc_free;
    this_repr->gc_cleanup = gc_cleanup;
    this_repr->get_storage_spec = get_storage_spec;
    this_repr->serialize = serialize;
    this_repr->deserialize = deserialize;
    return this_repr;
}
