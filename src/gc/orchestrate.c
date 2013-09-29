#include "moarvm.h"
#include <platform/threads.h>

/* If we have the job of doing GC for a thread, we add it to our work
 * list. */
static void add_work(MVMThreadContext *tc, MVMThreadContext *other) {
    if (tc->gc_work == NULL) {
        tc->gc_work_size = 16;
        tc->gc_work = malloc(tc->gc_work_size * sizeof(MVMWorkThread));
    }
    else if (tc->gc_work_count == tc->gc_work_size) {
        tc->gc_work_size *= 2;
        tc->gc_work = realloc(tc->gc_work, tc->gc_work_size * sizeof(MVMWorkThread));
    }
    tc->gc_work[tc->gc_work_count++].tc = other;
    MVM_store(&other->gc_owner_tc, tc);
}

/* Tries to signal a thread to interrupt, or else steal it. Return 1 iff 
 * we successfully interrupted it. */
static MVMuint32 signal_one_thread(MVMThreadContext *tc, MVMThreadContext *to_signal) {
    AO_t gc_status;
    MVMuint32 i;

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
            GCDEBUG_LOG(tc, MVM_GC_DEBUG_ORCHESTRATE,
                "didn't steal the gc work of thread %d because its gc_status was %d\n",
                to_signal->thread_id, gc_status);
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
        MVM_platform_thread_yield();
    }
}

static void try_steal_children(MVMThreadContext *tc, MVMThreadContext *target) {
    MVMuint32 i, count = target->gc_work_count;
    AO_t tmp0 = 0;
    MVMWorkThread *to_try = calloc(count, sizeof(MVMWorkThread));

    /* Save a copy of our current workset. */
    memcpy(to_try, target->gc_work, count * sizeof(MVMWorkThread));
    /* Reset our counter so recursive calls use the same data. */
    /* Note: don't need an atomic store here since we'll be the thread using
     * this data anyway... */
    target->gc_work_count = 0;
    for (i = 0; i < count; i++) {
        MVMThreadContext *child = to_try[i].tc;

        if (MVM_try_steal(tc, child, tmp0)) {
            GCDEBUG_LOG(tc, MVM_GC_DEBUG_ORCHESTRATE,
                "stole the gc work of child thread %d\n", child->thread_id);
            add_work(tc, child);
            /* Recursively try to steal its children.. */
            try_steal_children(tc, child);
        }
    }
    free(to_try);
}

#define register_gc_run_thread_id(thread) \
    MVM_store(&(thread)->gc_thread_id, MVM_incr(&(thread)->instance->gc_thread_id))

/* Processes the gc_work queue, attempting to steal the children.  They are
 * guaranteed to exist because every thread lasts at least 1 GC run, and the
 * entry stays in this table no more than 1 GC run. */
static void register_for_gc_run(MVMThreadContext *tc) {
    /* Register for a GC thread ID. */
    register_gc_run_thread_id(tc);
    /* Recursively steal our children. */
    try_steal_children(tc, tc);
    /* Add ourself. */
    add_work(tc, tc);

    /* Increment the finish counter to signify we're registering in
     * the GC run and for the other to wait for us to finish that
     * phase. */
    MVM_incr(&tc->instance->gc_finish);
}

/* This is called when the allocator finds it has run out of memory and wants
 * to trigger a GC run. In this case, it's possible (probable, really) that it
 * will need to do that triggering, notifying other running threads that the
 * time has come to GC. */
void MVM_gc_enter_from_allocator(MVMThreadContext *tc) {
    AO_t tmp0 = 0;
    GCDEBUG_LOG(tc, MVM_GC_DEBUG_ORCHESTRATE, "Entered from allocator\n");

    /* Meed to wait for other threads to reset their gc_status from
     * the last GC run. */
    while (    MVM_load(&tc->instance->gc_ack)
            && !MVM_load(&tc->instance->gc_start))
        MVM_platform_yield();

    /* Try to start the GC run. We first must be able to interrupt ourself,
     * since if we increment gc_start first, there could be a race from
     * another thread who enters_from_allocator to interrupt us if it is
     * the thread who spawned us. */
    if (    MVM_try_interrupt(tc, tc, tmp0)
         && MVM_incr(&tc->instance->gc_start) == 0) {

        MVMThread *thread_obj = (MVMThread *)MVM_load(&tc->instance->threads);

        /* We are the winner of the GC starting race. This gives us some
         * extra responsibilities as well as doing the usual things.
         * First, increment GC sequence number. */
        tmp0 = MVM_incr(&tc->instance->gc_seq_number);
        GCDEBUG_LOG(tc, MVM_GC_DEBUG_ORCHESTRATE,
            "GC thread elected coordinator: starting gc seq %d\n", tmp0 + 1);

        /* Free any STables that have been marked for
         * deletion. It's okay for us to muck around in
         * another thread's fromspace while it's mutating
         * tospace, really. */
        MVM_gc_collect_free_stables(tc);

        register_for_gc_run(tc);

        /* Iterate the linked list of thread objects.  Even though we're racing
         * other threads who create new child threads, it's okay because those
         * children will end up being interrupted or stolen also by their
         * parents, which we are guaranteed to know about here (recursively). */
        do {
            MVMThreadContext *other;
            /* Skip this one if the thread context on the thread object has
             * been nulled, or if it's ourself, or if it's been destroyed, or
             * if it's already been interrupted or stolen. */
            if (    !(other = thread_obj->body.tc)
                 || tc == other
                 || MVM_load(&thread_obj->body.stage)
                        == MVM_thread_stage_destroyed
                 || (tmp0 = MVM_load(&other->gc_status)) == MVMGCStatus_STOLEN
                 || tmp0 == MVMGCStatus_INTERRUPT
            )
                continue;
            /* Must pre-increment it in case it gets interrupted and joins the
             * run after we signal it but before we increment gc_start. */
            MVM_incr(&tc->instance->gc_start);
            if (!signal_one_thread(tc, thread_obj->body.tc)) {
                tmp0 = MVM_decr(&tc->instance->gc_start);
                GCDEBUG_LOG(tc, MVM_GC_DEBUG_GCSTART,
                    "decr gc_start to %d\n", tmp0 - 1);
            }
            else {
                GCDEBUG_LOG(tc, MVM_GC_DEBUG_ORCHESTRATE,
                    "interrupted thread %d\n", thread_obj->body.tc->thread_id);
            }
        } while ((thread_obj = thread_obj->body.next));

        MVM_gc_run_gc(tc, MVMGCWhatToDo_All);
    }
    else {
        /* Another thread beat us to starting the GC sync process. Thus, act as
         * if we were interrupted to GC. */
        GCDEBUG_LOG(tc, MVM_GC_DEBUG_ORCHESTRATE, "Lost coordinator election\n");

        /* Also, we might have been successfully interrupted by another thread
         * anyway, but they are handling our gc_start increment in that case,
         * but if we were the ones who interrupted ourself (if gc_status was
         * running (and so the try_interrupt succeeded)), we still don't need
         * to increment gc_start, since we attempted to in the 2nd part of the
         * above condition. */

        register_for_gc_run(tc);
        MVM_gc_run_gc(tc, MVMGCWhatToDo_NoInstance);
    }
}

/* This is called when a thread hits an interrupt at a GC safe point. This means
 * that another thread is already trying to start a GC run, so we don't need to
 * try and do that, just enlist in the run. */
void MVM_gc_enter_from_interrupt(MVMThreadContext *tc) {
    GCDEBUG_LOG(tc, MVM_GC_DEBUG_ORCHESTRATE, "Entered from interrupt\n");

    /* Meed to wait for other threads to reset their gc_status from
     * the last GC run. */
    while (    MVM_load(&tc->instance->gc_ack)
            && !MVM_load(&tc->instance->gc_start))
        MVM_platform_yield();

    register_for_gc_run(tc);
    MVM_gc_run_gc(tc, MVMGCWhatToDo_NoInstance);
}

#define force_full_gc(tc) MVM_store(&(tc)->instance->gc_seq_number, MVM_GC_GEN2_RATIO - 1)

/* Run the global destruction phase. */
void MVM_gc_global_destruction(MVMThreadContext *tc) {
    char *nursery_tmp;

    /* Must wait until we're the only thread... */
    if (tc->instance->num_user_threads) {
        do {
            if (MVM_load(&tc->instance->gc_morgue_thread_count)) {
                /* need to run the GC to clean up those threads. */
                force_full_gc(tc);
                MVM_gc_enter_from_allocator(tc);
            }
            /* Now we need to join the GC run if some other thread
             * has signaled us. */
            GC_SYNC_POINT(tc);
        MVM_platform_thread_yield();
        } while (tc->instance->num_user_threads);
        /* must run once again to actually destroy them... */
        force_full_gc(tc);
        MVM_gc_enter_from_allocator(tc);
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
