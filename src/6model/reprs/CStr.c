#include <moarvm.h>
#include "CStr.h"

#define ALIGNOF(type) \
    ((MVMuint16)offsetof(struct { char dummy; type member; }, member))

typedef MVMObject * (* wrap_object_t) (MVMThreadContext *tc, void *obj);
typedef MVMObject * (* create_stable_t) (MVMThreadContext *tc, MVMREPROps *REPR, MVMObject *HOW);

/* This representation's function pointer table. */
static MVMREPROps *this_repr;

/* Some functions we have to get references to. */
static wrap_object_t   wrap_object_func;
static create_stable_t create_stable_func;

static void set_str(MVMThreadContext *tc, MVMSTable *st,  MVMObject *root, void *data, MVMString *value) {
    /* TODO */
#if 0
    MVMCStrBody *body = (MVMCStrBody *) data;
    MVMObject *old_ctx, *cappy, *meth, *enc_pmc;
    STRING *enc;
    STR_VTABLE *encoding;

    if(body->cstr)
        mem_sys_free(body->cstr);

    /* Look up "encoding" method. */
    meth = VTABLE_find_method(tc, st->WHAT,
        Parrot_str_new_constant(tc, "encoding"));
    if (MVMObject_IS_NULL(meth))
        Parrot_ex_throw_from_c_args(tc, NULL, EXCEPTION_INVALID_OPERATION,
            "MVMCStr representation expects an 'encoding' method, specifying the encoding");

    old_ctx = Parrot_pcc_get_signature(tc, CURRENT_CONTEXT(tc));
    cappy   = Parrot_pmc_new(tc, enum_class_CallContext);
    VTABLE_push_pmc(tc, cappy, st->WHAT);
    Parrot_pcc_invoke_from_sig_object(tc, meth, cappy);
    cappy = Parrot_pcc_get_signature(tc, CURRENT_CONTEXT(tc));
    Parrot_pcc_set_signature(tc, CURRENT_CONTEXT(tc), old_ctx);
    enc_pmc = decontainerize(tc, VTABLE_get_pmc_keyed_int(tc, cappy, 0));
    enc = REPR(enc_pmc)->box_funcs->get_str(tc, STABLE(enc_pmc), OBJECT_BODY(enc_pmc));

    if (STRING_equal(tc, enc, Parrot_str_new_constant(tc, "utf8")))
        encoding = Parrot_utf8_encoding_ptr;
    else if (STRING_equal(tc, enc, Parrot_str_new_constant(tc, "utf16")))
        encoding = Parrot_utf16_encoding_ptr;
    else if (STRING_equal(tc, enc, Parrot_str_new_constant(tc, "ascii")))
        encoding = Parrot_ascii_encoding_ptr;
    else
        Parrot_ex_throw_from_c_args(tc, NULL, EXCEPTION_INVALID_OPERATION,
            "Unknown encoding passed to MVMCStr representation");

    body->cstr = Parrot_str_to_encoded_cstring(tc, value, encoding);
#endif
}

/* Creates a new type object of this representation, and associates it with
 * the given HOW. */
static MVMObject *type_object_for(MVMThreadContext *tc, MVMObject *HOW) {
    /* TODO: Create REPR data structure and hang it off the MVMSTable.
     *       Don't need any REPR data yet. */

    MVMSTable *st = MVM_gc_allocate_stable(tc, this_repr, HOW);
    
    MVMROOT(tc, st, {
        MVMObject *obj = MVM_gc_allocate_type_object(tc, st);

        MVM_ASSIGN_REF(tc, st, st->WHAT, obj);
        st->size = sizeof(MVMCStr);
        /* FIXME??? */
        st->WHAT = wrap_object_func(tc, obj);
    });

    return st->WHAT;
}

/* Composes the representation. */
static void compose(MVMThreadContext *tc, MVMSTable *st, MVMObject *repr_info) {
    /* TODO: move encoding stuff into here */
}

/* Creates a new instance based on the type object. */
static MVMObject * allocate(MVMThreadContext *tc, MVMSTable *st) {
    MVMCStr *obj = calloc(1, sizeof(MVMCStr));
    obj->common.st = st;
    return wrap_object_func(tc, obj);
}

static void initialize(MVMThreadContext *tc, MVMSTable *st,  MVMObject *root, void *data) {
    (void)tc, (void)st, (void)root, (void)data;
}

/* Called by the VM in order to free memory associated with this object. */
static void gc_free(MVMThreadContext *tc, MVMObject *obj) {
    MVMCStr *str = (MVMCStr *)obj;
    if (str->body.cstr) {
        free(str->body.cstr);
        str->body.cstr = NULL;
    }
}

/* Gets the storage specification for this representation. */
static MVMStorageSpec get_storage_spec(MVMThreadContext *tc, MVMSTable *st) {
    MVMStorageSpec spec;
    spec.inlineable = MVM_STORAGE_SPEC_REFERENCE;
    spec.boxed_primitive = MVM_STORAGE_SPEC_BP_STR;
    spec.can_box = 0;
    spec.bits = sizeof(MVMCStrBody) * 8;
    spec.align = ALIGNOF(MVMCStrBody);
    return spec;
}

MVMREPROps *MVMCStr_initialize(MVMThreadContext *tc,
        wrap_object_t wrap_object_func_ptr,
        create_stable_t create_stable_func_ptr) {
    /* Stash away functions passed wrapping functions. */
    wrap_object_func = wrap_object_func_ptr;
    create_stable_func = create_stable_func_ptr;

    /* Allocate and populate the representation function table. */
    this_repr = calloc(1, sizeof(MVMREPROps));
    this_repr->type_object_for  = type_object_for;
    this_repr->compose          = compose;
    this_repr->allocate         = allocate;
    this_repr->initialize       = initialize;
    this_repr->gc_free          = gc_free;
    this_repr->get_storage_spec = get_storage_spec;
    this_repr->box_funcs = calloc(1, sizeof(MVMREPROps_Boxing));
    this_repr->box_funcs->set_str = set_str;

    return this_repr;
}
