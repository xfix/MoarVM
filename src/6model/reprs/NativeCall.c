#include <moarvm.h>
#include "NativeCall.h"

#define ALIGNOF(type) \
    ((MVMuint16)offsetof(struct { char dummy; type member; }, member))

typedef MVMObject * (* wrap_object_t) (MVMThreadContext *tc, void *obj);
typedef MVMObject * (* create_stable_t) (MVMThreadContext *tc, MVMREPROps *REPR, MVMObject *HOW);

/* This representation's function pointer table. */
static MVMREPROps *this_repr;

/* Some functions we have to get references to. */
static wrap_object_t   wrap_object_func;
static create_stable_t create_stable_func;

/* Creates a new type object of this representation, and associates it with
 * the given HOW. */
static MVMObject * type_object_for(MVMThreadContext *tc, MVMObject *HOW) {
    MVMSTable *st = MVM_gc_allocate_stable(tc, this_repr, HOW);

    MVMROOT(tc, st, {
        MVMObject *obj = MVM_gc_allocate_type_object(tc, st);

        MVM_ASSIGN_REF(tc, st, st->WHAT, obj);
        st->size = sizeof(MVMNativeCall);
        /* FIXME??? */
        st->WHAT = wrap_object_func(tc, obj);
    });

    return st->WHAT;
}

/* Composes the representation. */
static void compose(MVMThreadContext *tc, MVMSTable *st, MVMObject *repr_info) {
    /* Nothing to do. */
    (void)tc, (void)st, (void)repr_info;
}

/* Creates a new instance based on the type object. */
static MVMObject * allocate(MVMThreadContext *tc, MVMSTable *st) {
    MVMNativeCall *obj = calloc(1, sizeof(MVMNativeCall));
    obj->common.st = st;
    return wrap_object_func(tc, obj);
}

/* Initialize a new instance. */
static void initialize(MVMThreadContext *tc, MVMSTable *st, MVMObject *root, void *data) {
    /* Nothing to do here. */
    (void)tc, (void)st, (void)root, (void)data;
}

/* Copies to the body of one object to another. */
static void copy_to(MVMThreadContext *tc, MVMSTable *st, void *src, MVMObject *dest_root, void *dest) {
    /* FIXME: use dest_root? */

    MVMNativeCallBody *src_body = (MVMNativeCallBody *)src;
    MVMNativeCallBody *dest_body = (MVMNativeCallBody *)dest;

    /* Need a fresh handle for resource management purposes. */
    if (src_body->lib_name) {
        dest_body->lib_name = malloc(strlen(src_body->lib_name) + 1);
        strcpy(dest_body->lib_name, src_body->lib_name);
        dest_body->lib_handle = dlLoadLibrary(dest_body->lib_name);
    }

    /* Rest is just simple copying. */
    dest_body->entry_point = src_body->entry_point;
    dest_body->convention = src_body->convention;
    dest_body->num_args = src_body->num_args;
    if (src_body->arg_types) {
        dest_body->arg_types = malloc(sizeof(MVMuint16) * (src_body->num_args ? src_body->num_args : 1));
        memcpy(dest_body->arg_types, src_body->arg_types, src_body->num_args * sizeof(MVMuint16));
    }
    dest_body->ret_type = src_body->ret_type;
}

/* This is called to do any cleanup of resources when an object gets
 * embedded inside another one. Never called on a top-level object. */
static void gc_cleanup(MVMThreadContext *tc, MVMSTable *st, void *data) {
    (void)st;

    MVMNativeCallBody *body = (MVMNativeCallBody *)data;
    if (body->lib_name)
        free(body->lib_name);
    if (body->lib_handle)
        dlFreeLibrary(body->lib_handle);
    if (body->arg_types)
        free(body->arg_types);
    if (body->arg_info)
        free(body->arg_info);
}

/* This Parrot-specific addition to the API is used to free an object. */
static void gc_free(MVMThreadContext *tc, MVMObject *obj) {
    MVMNativeCall *nc = (MVMNativeCall *)obj;
    gc_cleanup(tc, nc->common.st, &nc->body);
}

static void gc_mark(MVMThreadContext *tc, MVMSTable *st, void *data, MVMGCWorklist *worklist) {
    MVMNativeCallBody *body = (MVMNativeCallBody *)data;

    if (body->arg_info) {
        MVMuint16 i;
        for (i = 0; i < body->num_args; i++) {
            if (body->arg_info[i])
                MVM_gc_worklist_add(tc, worklist, body->arg_info[i]);
        }
    }
}

/* Gets the storage specification for this representation. */
static MVMStorageSpec get_storage_spec(MVMThreadContext *tc, MVMSTable *st) {
    MVMStorageSpec spec;
    spec.inlineable = MVM_STORAGE_SPEC_INLINED;
    spec.bits = sizeof(MVMNativeCallBody) * 8;
    spec.align = ALIGNOF(MVMNativeCallBody);
    spec.boxed_primitive = MVM_STORAGE_SPEC_BP_NONE;
    spec.can_box = 0;
    return spec;
}

/* We can't actually serialize the handle, but since this REPR gets inlined
 * we just do nothing here since it may well have never been opened. Various
 * more involved approaches are possible... */
static void serialize(MVMThreadContext *tc, MVMSTable *st, void *data, MVMSerializationWriter *writer) {
    (void)tc, (void)st, (void)data, (void)writer;
}
static void deserialize(MVMThreadContext *tc, MVMSTable *st, MVMObject *root, void *data, MVMSerializationReader *reader) {
    (void)tc, (void)st, (void)root, (void)data, (void)reader;
}

/* Initializes the MVMNativeCall representation. */
MVMREPROps * MVMNativeCall_initialize(MVMThreadContext *tc,
        wrap_object_t wrap_object_func_ptr,
        create_stable_t create_stable_func_ptr) {
    /* Stash away functions passed wrapping functions. */
    wrap_object_func = wrap_object_func_ptr;
    create_stable_func = create_stable_func_ptr;

    /* Allocate and populate the representation function table. */
    this_repr = calloc(1, sizeof(MVMREPROps));
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
