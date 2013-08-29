/* Body of a CPointer. */
struct MVMCPointerBody {
    void *ptr;
};

/* This is how an instance with the CPointer representation looks. */
struct MVMCPointer {
    MVMObject common;
    MVMCPointerBody body;
};

/* Initializes the CPointer REPR. */
MVMREPROps * MVMCPointer_initialize(MVMThreadContext *tc,
        MVMObject * (* wrap_object_func_ptr) (MVMThreadContext *tc, void *obj),
        MVMObject * (* create_stable_func_ptr) (MVMThreadContext *tc, MVMREPROps *REPR, MVMObject *HOW));
