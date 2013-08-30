#include <moarvm.h>
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

/* Creates a new type object of this representation, and associates it with
 * the given HOW. */
static MVMObject * type_object_for(MVMThreadContext *tc, MVMObject *HOW) {
    MVMSTable        *st;
    MVMObject        *obj;

    st = MVM_gc_allocate_stable(tc, this_repr, HOW);
    MVMROOT(tc, st, {
        obj = MVM_gc_allocate_type_object(tc, st);
        MVM_ASSIGN_REF(tc, st, st->WHAT, obj);
        st->size = sizeof(MVMCPointer);
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
    MVMCPointer *obj = calloc(1, sizeof(MVMCPointer));
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
    /* FIXME: ignore dest_root? */
    MVMCPointerBody *src_body = (MVMCPointerBody *)src;
    MVMCPointerBody *dest_body = (MVMCPointerBody *)dest;
    dest_body->ptr = src_body->ptr;
}

/* No need to free anything. */
static void gc_free(MVMThreadContext *tc, MVMObject *obj) {
    (void)tc, (void)obj;
}

/* Gets the storage specification for this representation. */
static MVMStorageSpec get_storage_spec(MVMThreadContext *tc, MVMSTable *st) {
    MVMStorageSpec spec;
    spec.inlineable      = MVM_STORAGE_SPEC_REFERENCE;
    spec.boxed_primitive = MVM_STORAGE_SPEC_BP_NONE;
    spec.can_box         = 0;
    spec.bits            = sizeof(MVMCPointerBody) * 8;
    spec.align           = ALIGNOF(MVMCPointerBody);
    return spec;
}

/* Initializes the CPointer representation. */
MVMREPROps * MVMCPointer_initialize(MVMThreadContext *tc,
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
    this_repr->gc_free = gc_free;
    this_repr->get_storage_spec = get_storage_spec;
    return this_repr;
}
