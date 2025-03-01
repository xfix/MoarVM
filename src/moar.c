#include "moar.h"

#define init_mutex(loc, name) do { \
    if ((init_stat = uv_mutex_init(&loc)) < 0) { \
        fprintf(stderr, "MoarVM: Initialization of " name " mutex failed\n    %s\n", \
            uv_strerror(init_stat)); \
        exit(1); \
	} \
} while (0)

/* Create a new instance of the VM. */
static void string_consts(MVMThreadContext *tc);
static void setup_std_handles(MVMThreadContext *tc);
MVMInstance * MVM_vm_create_instance(void) {
    MVMInstance *instance;
    char *spesh_log, *spesh_disable;
    int init_stat;

    /* Set up instance data structure. */
    instance = calloc(1, sizeof(MVMInstance));

    /* Create the main thread's ThreadContext and stash it. */
    instance->main_thread = MVM_tc_create(instance);
    instance->main_thread->thread_id = 1;

    /* No user threads when we start, and next thread to be created gets ID 2
     * (the main thread got ID 1). */
    instance->num_user_threads    = 0;
    MVM_store(&instance->next_user_thread_id, 2);

    /* Set up the permanent roots storage. */
    instance->num_permroots   = 0;
    instance->alloc_permroots = 16;
    instance->permroots       = malloc(sizeof(MVMCollectable **) * instance->alloc_permroots);
    init_mutex(instance->mutex_permroots, "permanent roots");

    /* Set up REPR registry mutex. */
    init_mutex(instance->mutex_repr_registry, "REPR registry");

    /* Set up HLL config mutex. */
    init_mutex(instance->mutex_hllconfigs, "hll configs");

    /* Set up DLL registry mutex. */
    init_mutex(instance->mutex_dll_registry, "REPR registry");

    /* Set up extension registry mutex. */
    init_mutex(instance->mutex_ext_registry, "extension registry");

    /* Set up extension op registry mutex. */
    init_mutex(instance->mutex_extop_registry, "extension op registry");

    /* Set up weak reference hash mutex. */
    init_mutex(instance->mutex_sc_weakhash, "sc weakhash");

    /* Set up loaded compunits hash mutex. */
    init_mutex(instance->mutex_loaded_compunits, "loaded compunits");

    /* Set up container registry mutex. */
    init_mutex(instance->mutex_container_registry, "container registry");

    /* Allocate all things during following setup steps directly in gen2, as
     * they will have program lifetime. */
    MVM_gc_allocate_gen2_default_set(instance->main_thread);

    init_mutex(instance->mutex_int_const_cache, "int constant cache");
    instance->int_const_cache = calloc(1, sizeof(MVMIntConstCache));

    /* Bootstrap 6model. It is assumed the GC will not be called during this. */
    MVM_6model_bootstrap(instance->main_thread);

    /* Fix up main thread's usecapture. */
    instance->main_thread->cur_usecapture = MVM_repr_alloc_init(instance->main_thread, instance->CallCapture);

    /* Initialize event loop thread starting mutex. */
    init_mutex(instance->mutex_event_loop_start, "event loop thread start");
    
    /* Create main thread object, and also make it the start of the all threads
     * linked list. */
    MVM_store(&instance->threads,
        (instance->main_thread->thread_obj = (MVMThread *)
            REPR(instance->boot_types.BOOTThread)->allocate(
                instance->main_thread, STABLE(instance->boot_types.BOOTThread))));
    instance->threads->body.stage = MVM_thread_stage_started;
    instance->threads->body.tc = instance->main_thread;
    instance->threads->body.thread_id = uv_thread_self();

    /* Create compiler registry */
    instance->compiler_registry = MVM_repr_alloc_init(instance->main_thread, instance->boot_types.BOOTHash);

    /* Set up compiler registr mutex. */
    init_mutex(instance->mutex_compiler_registry, "compiler registry");

    /* Create hll symbol tables */
    instance->hll_syms = MVM_repr_alloc_init(instance->main_thread, instance->boot_types.BOOTHash);

    /* Set up hll symbol tables mutex. */
    init_mutex(instance->mutex_hll_syms, "hll syms");

    /* Initialize string cclass handling. */
    MVM_string_cclass_init(instance->main_thread);

    /* Create callsite intern pool. */
    instance->callsite_interns = calloc(1, sizeof(MVMCallsiteInterns));
    init_mutex(instance->mutex_callsite_interns, "callsite interns");

    /* Mutex for spesh installations, and check if we've a file we
     * should log specializations to. */
    init_mutex(instance->mutex_spesh_install, "spesh installations");
    spesh_log = getenv("MVM_SPESH_LOG");
    if (spesh_log && strlen(spesh_log))
        instance->spesh_log_fh = fopen(spesh_log, "w");
    spesh_disable = getenv("MVM_SPESH_DISABLE");
    if (!spesh_disable || strlen(spesh_disable) == 0)
        instance->spesh_enabled = 1;

    /* Create std[in/out/err]. */
    setup_std_handles(instance->main_thread);

    /* Back to nursery allocation, now we're set up. */
    MVM_gc_allocate_gen2_default_clear(instance->main_thread);

    return instance;
}

/* Set up some standard file handles. */
static void setup_std_handles(MVMThreadContext *tc) {
    tc->instance->stdin_handle  = MVM_file_get_stdstream(tc, 0, 1);
    MVM_gc_root_add_permanent(tc, (MVMCollectable **)&tc->instance->stdin_handle);

    tc->instance->stdout_handle = MVM_file_get_stdstream(tc, 1, 0);
    MVM_gc_root_add_permanent(tc, (MVMCollectable **)&tc->instance->stdout_handle);

    tc->instance->stderr_handle = MVM_file_get_stdstream(tc, 2, 0);
    MVM_gc_root_add_permanent(tc, (MVMCollectable **)&tc->instance->stderr_handle);
}

/* This callback is passed to the interpreter code. It takes care of making
 * the initial invocation. */
static void toplevel_initial_invoke(MVMThreadContext *tc, void *data) {
    /* Dummy, 0-arg callsite. */
    static MVMCallsite no_arg_callsite = { NULL, 0, 0, 0 };

    /* Create initial frame, which sets up all of the interpreter state also. */
    MVM_frame_invoke(tc, (MVMStaticFrame *)data, &no_arg_callsite, NULL, NULL, NULL);
}

/* Loads bytecode from the specified file name and runs it. */
void MVM_vm_run_file(MVMInstance *instance, const char *filename) {
    MVMStaticFrame *start_frame;

    /* Map the compilation unit into memory and dissect it. */
    MVMThreadContext *tc = instance->main_thread;
    MVMCompUnit      *cu = MVM_cu_map_from_file(tc, filename);

    MVMROOT(tc, cu, {
        /* The call to MVM_string_utf8_decode() may allocate, invalidating the
           location cu->body.filename */
        MVMString *const str = MVM_string_utf8_decode(tc, instance->VMString, filename, strlen(filename));
        cu->body.filename = str;

        /* Run deserialization frame, if there is one. */
        if (cu->body.deserialize_frame) {
            MVM_interp_run(tc, &toplevel_initial_invoke, cu->body.deserialize_frame);
        }
    });

    /* Run the frame marked main, or if there is none then fall back to the
     * first frame. */
    start_frame = cu->body.main_frame ? cu->body.main_frame : cu->body.frames[0];
    MVM_interp_run(tc, &toplevel_initial_invoke, start_frame);
}

/* Loads bytecode from the specified file name and dumps it. */
void MVM_vm_dump_file(MVMInstance *instance, const char *filename) {
    /* Map the compilation unit into memory and dissect it. */
    MVMThreadContext *tc = instance->main_thread;
    MVMCompUnit      *cu = MVM_cu_map_from_file(tc, filename);
    char *dump = MVM_bytecode_dump(tc, cu);

    printf("%s", dump);
    free(dump);
}

/* Exits the process as quickly as is gracefully possible, respecting that
 * foreground threads should join first. Leaves all cleanup to the OS, as it
 * will be able to do it much more swiftly than we could. This is typically
 * not the right thing for embedding; see MVM_vm_destroy_instance for that. */
void MVM_vm_exit(MVMInstance *instance) {
    /* Join any foreground threads. */
    MVM_thread_join_foreground(instance->main_thread);

    /* Close any spesh log. */
    if (instance->spesh_log_fh)
        fclose(instance->spesh_log_fh);

    /* And, we're done. */
    exit(0);
}

/* Destroys a VM instance. This must be called only from the main thread. It
 * should clear up all resources and free all memory; in practice, it falls
 * short of this goal at the moment. */
void MVM_vm_destroy_instance(MVMInstance *instance) {
    /* Join any foreground threads. */
    MVM_thread_join_foreground(instance->main_thread);

    /* Run the GC global destruction phase. After this,
     * no 6model object pointers should be accessed. */
    MVM_gc_global_destruction(instance->main_thread);

    /* Cleanup REPR registry */
    uv_mutex_destroy(&instance->mutex_repr_registry);
    MVM_HASH_DESTROY(hash_handle, MVMReprRegistry, instance->repr_hash);
    MVM_checked_free_null(instance->repr_list);

    /* Clean up GC permanent roots related resources. */
    uv_mutex_destroy(&instance->mutex_permroots);
    MVM_checked_free_null(instance->permroots);

    /* Clean up Hash of HLLConfig. */
    uv_mutex_destroy(&instance->mutex_hllconfigs);
    MVM_HASH_DESTROY(hash_handle, MVMHLLConfig, instance->compiler_hll_configs);
    MVM_HASH_DESTROY(hash_handle, MVMHLLConfig, instance->compilee_hll_configs);

    /* Clean up Hash of DLLs. */
    uv_mutex_destroy(&instance->mutex_dll_registry);
    MVM_HASH_DESTROY(hash_handle, MVMDLLRegistry, instance->dll_registry);

    /* Clean up Hash of extensions. */
    uv_mutex_destroy(&instance->mutex_ext_registry);
    MVM_HASH_DESTROY(hash_handle, MVMExtRegistry, instance->ext_registry);

    /* Clean up Hash of extension ops. */
    uv_mutex_destroy(&instance->mutex_extop_registry);
    MVM_HASH_DESTROY(hash_handle, MVMExtOpRegistry, instance->extop_registry);

    /* Clean up Hash of all known serialization contexts. */
    uv_mutex_destroy(&instance->mutex_sc_weakhash);
    MVM_HASH_DESTROY(hash_handle, MVMSerializationContextBody, instance->sc_weakhash);

    /* Clean up Hash of filenames of compunits loaded from disk. */
    uv_mutex_destroy(&instance->mutex_loaded_compunits);
    MVM_HASH_DESTROY(hash_handle, MVMLoadedCompUnitName, instance->loaded_compunits);

    /* Clean up Container registry. */
    uv_mutex_destroy(&instance->mutex_container_registry);
    MVM_HASH_DESTROY(hash_handle, MVMContainerRegistry, instance->container_registry);

    /* Clean up Hash of compiler objects keyed by name. */
    uv_mutex_destroy(&instance->mutex_compiler_registry);

    /* Clean up Hash of hashes of symbol tables per hll. */
    uv_mutex_destroy(&instance->mutex_hll_syms);

    /* Clean up spesh install mutex and close any log. */
    uv_mutex_destroy(&instance->mutex_spesh_install);
    if (instance->spesh_log_fh)
        fclose(instance->spesh_log_fh);

    /* Clean up event loop starting mutex. */
    uv_mutex_destroy(&instance->mutex_event_loop_start);

    /* Destroy main thread contexts. */
    MVM_tc_destroy(instance->main_thread);

    /* Clear up VM instance memory. */
    free(instance);
}
