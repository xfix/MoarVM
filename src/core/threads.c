#include "moarvm.h"
#include <platform/threads.h>

/* Temporary structure for passing data to thread start. */
typedef struct {
    MVMThreadContext *tc;
    MVMFrame         *caller;
    /* was formerly the MVMCode invokee representing the object, but now
     * it is the MVMThread (which in turns has a handle to the invokee). */
    MVMObject        *thread_obj;
    MVMCallsite       no_arg_callsite;
} ThreadStart;

/* This callback is passed to the interpreter code. It takes care of making
 * the initial invocation of the thread code. */
static void thread_initial_invoke(MVMThreadContext *tc, void *data) {
    /* The passed data is simply the code object to invoke. */
    ThreadStart *ts = (ThreadStart *)data;
    MVMThread *thread = (MVMThread *)ts->thread_obj;
    MVMObject *invokee = thread->body.invokee;

    thread->body.invokee = NULL;

    /* Dummy, 0-arg callsite. */
    ts->no_arg_callsite.arg_flags = NULL;
    ts->no_arg_callsite.arg_count = 0;
    ts->no_arg_callsite.num_pos   = 0;

    /* Create initial frame, which sets up all of the interpreter state also. */
    STABLE(invokee)->invoke(tc, invokee, &ts->no_arg_callsite, NULL);

    /* This frame should be marked as the thread entry frame, so that any
     * return from it will cause us to drop out of the interpreter and end
     * the thread. */
    tc->thread_entry_frame = tc->cur_frame;
}

/* This callback handles starting execution of a thread. */
static void start_thread(void *data) {
    ThreadStart *ts = (ThreadStart *)data;
    MVMThreadContext *tc = ts->tc;

    /* Set the current frame in the thread to be the initial caller;
     * the ref count for this was incremented in the original thread. */
    tc->cur_frame = ts->caller;

    /* wait for the GC to finish if it's not finished stealing us. */
    MVM_gc_mark_thread_unblocked(tc);
    tc->thread_obj->body.stage = MVM_thread_stage_started;

    /* Enter the interpreter, to run code. */
    MVM_interp_run(tc, &thread_initial_invoke, ts);

    /* mark as exited, so the GC will know to clear our stuff. */
    MVM_store(&tc->thread_obj->body.stage, MVM_thread_stage_exited);
    GCDEBUG_LOG(tc, MVM_GC_DEBUG_ORCHESTRATE, "Thread %d XX(%d) : marking self EXITED\n");
    MVM_incr(&tc->instance->gc_morgue_thread_count);

    /* Now we're done, decrement the reference count of the caller. */
    MVM_frame_dec_ref(tc, ts->caller);

    /* hopefully pop the ts->thread_obj temp */
    MVM_gc_root_temp_pop(tc);

    /* Mark ourselves as dying, so that another thread will take care
     * of GC-ing our objects and cleaning up our thread context. */
    MVM_gc_mark_thread_blocked(tc);

    GCDEBUG_LOG(tc, MVM_GC_DEBUG_ORCHESTRATE, "Thread %d XX(%d) : thread EXITING.\n");

    free(ts);

    /* Exit the thread, now it's completed. */
#ifdef _WIN32
    ExitThread(0);
#else
    pthread_exit(NULL);
#endif
}

MVMObject * MVM_thread_start(MVMThreadContext *tc, MVMObject *invokee, MVMObject *result_type) {
    int status;
    ThreadStart *ts;
    MVMObject *child_obj;

    /* Create a thread object to wrap it up in. */
    MVM_gc_root_temp_push(tc, (MVMCollectable **)&invokee);
    child_obj = REPR(result_type)->allocate(tc, STABLE(result_type));
    MVM_gc_root_temp_pop(tc);
    if (REPR(child_obj)->ID == MVM_REPR_ID_MVMThread) {
        MVMThread *child = (MVMThread *)child_obj;
        MVMThread * volatile *threads;

        /* Create a new thread context and set it up. */
        MVMThreadContext *child_tc = MVM_tc_create(tc->instance);

        MVM_incr(&tc->instance->num_user_threads);
        child->body.tc = child_tc;
        MVM_ASSIGN_REF(tc, child, child->body.invokee, invokee);
        child_tc->thread_obj = child;
        child_tc->thread_id = MVM_incr(&tc->instance->next_user_thread_id);

        /* Create the thread. Note that we take a reference to the current frame,
         * since it must survive to be the dynamic scope of where the thread was
         * started, and there's no promises that the thread won't start before
         * the code creating the thread returns. The count is decremented when
         * the thread is done. */
        ts = malloc(sizeof(ThreadStart));
        ts->tc = child_tc;
        ts->caller = MVM_frame_inc_ref(tc, tc->cur_frame);
        ts->thread_obj = child_obj;

        /* push this to the *child* tc's temp roots. */
        MVM_gc_root_temp_push(child_tc, (MVMCollectable **)&ts->thread_obj);

        /* Set us up to perform the child's first GC if necessary. */
        MVM_gc_register_child_thread(tc, child_tc);

        /* push to starting threads list */
        threads = &tc->instance->threads;
        do {
            MVMThread *curr = *threads;
            MVM_ASSIGN_REF(tc, child, child->body.next, curr);
        } while (MVM_casptr(threads, child->body.next, child) != child->body.next);

        status = uv_thread_create(&child->body.thread, &start_thread, ts);

        if (status < 0) {
            MVM_panic(MVM_exitcode_compunit, "Could not spawn thread: errorcode %d", status);
        }
    }
    else {
        MVM_exception_throw_adhoc(tc,
            "Thread result type must have representation MVMThread");
    }

    return child_obj;
}

void MVM_thread_join(MVMThreadContext *tc, MVMObject *thread_obj) {
    if (REPR(thread_obj)->ID == MVM_REPR_ID_MVMThread) {
        MVMThread *thread = (MVMThread *)thread_obj;
        uv_thread_t *uv_thread = &thread->body.thread;
        int status;
        if (MVM_load(&((MVMThread *)thread_obj)->body.stage) < MVM_thread_stage_exited) {
            MVM_gc_mark_thread_blocked(tc);
            status = uv_thread_join(uv_thread);
            MVM_gc_mark_thread_unblocked(tc);
            if (status < 0)
                MVM_panic(MVM_exitcode_compunit, "Could not join thread: errorcode %d", status);
        }
        else { /* the target already ended */
        }
    }
    else {
        MVM_exception_throw_adhoc(tc,
            "Thread handle passed to join must have representation MVMThread");
    }
}
