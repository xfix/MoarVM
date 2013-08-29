struct MVMCStrBody {
    char *cstr;
};

/* This is how an instance with the CStr representation looks. */
struct MVMCStr {
    MVMObject common;
    MVMCStrBody body;
};

/* Not needed yet.
typedef struct {
} CStrREPRData;*/

MVMREPROps *MVMCStr_initialize(MVMThreadContext *tc,
        MVMObject * (* wrap_object_func_ptr) (MVMThreadContext *tc, void *obj),
        MVMObject * (* create_stable_func_ptr) (MVMThreadContext *tc, MVMREPROps *REPR, MVMObject *HOW));
