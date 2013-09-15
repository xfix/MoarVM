#include "moarvm.h"
#include <platform/threads.h>

/* If we have the job of doing GC for a thread, we add it to our work
 * list. */
static void add_work(MVMThreadContext *tc, MVMThreadContext *stolen) {
    if (tc->gc_work == NULL) {
        tc->gc_work_size = 16;
        tc->gc_work = malloc(tc->gc_work_size * sizeof(MVMWorkThread));
    }
    else if (tc->gc_work_count == tc->gc_work_size) {
        tc->gc_work_size *= 2;
        tc->gc_work = realloc(tc->gc_work, tc->gc_work_size * sizeof(MVMWorkThread));
    }
    tc->gc_work[tc->gc_work_count++].tc = stolen;
    MVM_store(&stolen->gc_owner_tc, tc);
}

/* Tries to signal a thread to interrupt, or else steal it. */
static MVMuint32 signal_one_thread(MVMThreadContext *tc, MVMThreadContext *to_signal) {
    AO_t gc_status;

    /* Loop here since we may not succeed first time (e.g. the status of the
     * thread may change between the two ways we try to twiddle it). */
    while (1) {
        if (MVM_try_interrupt(tc, to_signal, gc_status))
            return 1;
        if (MVM_try_steal(tc, to_signal, gc_status)) {
            GCDEBUG_LOG(tc, MVM_GC_DEBUG_ORCHESTRATE,
                "stole the gc work of thread %d\n", to_signal->thread_id);
            add_work(tc, to_signal);
            return 0;
        }
        if (gc_status == MVMGCStatus_STOLEN
                || gc_status == MVMGCStatus_INTERRUPT)
            return 0;
    }
}

/* Called by a thread to indicate it is about to enter a blocking operation.
 * This tells any thread that is coordinating a GC run that this thread will
 * be unable to participate. */
void MVM_gc_mark_thread_blocked(MVMThreadContext *tc) {
    AO_t gc_status;

    GCDEBUG_LOG(tc, MVM_GC_DEBUG_ORCHESTRATE, "marking blocked\n");
    /* This may need more than one attempt. */
    while (1) {
        /* Try to set it from running to unable - the common case. */
        if (MVM_try_mark_blocked(tc, tc, gc_status))
            return;

        /* The only way this can fail is if another thread just decided we're to
         * participate in a GC run. */
        if (gc_status == MVMGCStatus_INTERRUPT)
            MVM_gc_enter_from_interrupt(tc);
        else
            MVM_panic(MVM_exitcode_gcorch, "Invalid GC status %d observed; aborting", gc_status);
    }
}

/* Called by a thread to indicate it has completed a block operation and is
 * thus able to particpate in a GC run again. Note that this case needs some
 * special handling if it comes out of this mode when a GC run is taking place. */
void MVM_gc_mark_thread_unblocked(MVMThreadContext *tc) {
    AO_t gc_status;

    GCDEBUG_LOG(tc, MVM_GC_DEBUG_ORCHESTRATE, "marking unblocked\n");
    while (!MVM_try_mark_unblocked(tc, tc, gc_status)) {
        /* We can't, presumably because a GC run is going on. We should wait
         * for that to finish before we go on, but without chewing CPU. */
        MVM_platform_yield();
    }
}

/* This is called when the allocator finds it has run out of memory and wants
 * to trigger a GC run. In this case, it's possible (probable, really) that it
 * will need to do that triggering, notifying other running threads that the
 * time has come to GC. */
void MVM_gc_enter_from_allocator(MVMThreadContext *tc) {
    AO_t tmp0 = 0;
    GCDEBUG_LOG(tc, MVM_GC_DEBUG_ORCHESTRATE, "Entered from allocator\n");

    /* Try to start the GC run. */
    if (MVM_cas(&tc->instance->gc_start, 0, 1) == 0) {
        MVMThread *thread_obj = (MVMThread *)MVM_load(&tc->instance->threads);

        /* We are the winner of the GC starting race. This gives us some
         * extra responsibilities as well as doing the usual things.
         * First, increment GC sequence number. */
        tmp0 = MVM_incr(&tc->instance->gc_seq_number);
        GCDEBUG_LOG(tc, MVM_GC_DEBUG_ORCHESTRATE,
            "GC thread elected coordinator: starting gc seq %d\n", tmp0 + 1);

        /* Meed to wait for other threads to reset their gc_status from
         * the last GC run. */
        while (MVM_load(&tc->instance->gc_ack))
            MVM_platform_yield();

        /* Iterate the linked list of thread objects.  Even though we're racing
         * other threads who create new child threads, it's okay because those
         * children will end up being interrupted or stolen also by their
         * parents, which we are guaranteed to know about here (recursively). */
        do {
            if (tc == thread_obj->body.tc) continue;
            MVM_incr(&tc->instance->gc_start);
            if (!signal_one_thread(tc, thread_obj->body.tc))
                MVM_decr(&tc->instance->gc_start);
        } while ((thread_obj = thread_obj->body.next));
        MVM_gc_run_gc(tc, MVMGCWhatToDo_All);
    }
    else {
        /* Another thread beat us to starting the GC sync process. Thus, act as
         * if we were interrupted to GC. */
        GCDEBUG_LOG(tc, MVM_GC_DEBUG_ORCHESTRATE, "Lost coordinator election\n");

        /* Need to increment gc_start iff we're able to interrupt ourself,
         * since we need to simulate those conditions thoroughly. */
        MVM_incr(&tc->instance->gc_start);
        if (!MVM_try_interrupt(tc, tc, tmp0))
            MVM_decr(&tc->instance->gc_start);
        MVM_gc_run_gc(tc, MVMGCWhatToDo_NoInstance);
    }
}

/* This is called when a thread hits an interrupt at a GC safe point. This means
 * that another thread is already trying to start a GC run, so we don't need to
 * try and do that, just enlist in the run. */
void MVM_gc_enter_from_interrupt(MVMThreadContext *tc) {
    GCDEBUG_LOG(tc, MVM_GC_DEBUG_ORCHESTRATE, "Entered from interrupt\n");
    MVM_gc_run_gc(tc, MVMGCWhatToDo_NoInstance);
}

/* Run the global destruction phase. */
void MVM_gc_global_destruction(MVMThreadContext *tc) {
    char *nursery_tmp;

    /* Must wait until we're the only thread... */
    while (tc->instance->num_user_threads) {
        if (MVM_load(&tc->instance->gc_morgue_thread_count)) {
            /* need to run the GC to clean up those threads. */
            MVM_gc_enter_from_allocator(tc);
        }
        GC_SYNC_POINT(tc);
        MVM_platform_yield();
    }

    /* Fake a nursery collection run by swapping the semi-
     * space nurseries. */
    nursery_tmp = tc->nursery_fromspace;
    tc->nursery_fromspace = tc->nursery_tospace;
    tc->nursery_tospace = nursery_tmp;

    /* Run the objects' finalizers */
    MVM_gc_collect_free_nursery_uncopied(tc, tc->nursery_alloc);
    MVM_gc_collect_cleanup_gen2roots(tc);
    MVM_gc_collect_free_gen2_unmarked(tc);
    MVM_gc_collect_free_stables(tc);
}

/* Register a child thread with the parent thread's GC work queue. */
void MVM_gc_register_child_thread(MVMThreadContext *tc, MVMThreadContext *child_tc) {
    /* Add child to our work queue. */
    add_work(tc, child_tc);

    /* Mark child as blocked, so that we can attempt to steal it when the GC
     * runs, unless it successfully unblocks itself first. */
    MVM_store(&child_tc->gc_status, MVMGCStatus_UNABLE);
}
