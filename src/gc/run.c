#include "moarvm.h"

/* Processes the gc_work queue, attempting to steal the children.  They are
 * guaranteed to exist because every thread lasts at least 1 GC run, and the
 * entry stays in this table no more than 1 GC run. */
static void register_for_gc_run(MVMThreadContext *tc) {
    MVMuint32 i, count = 0;
    AO_t tmp0 = 0;

    if (tc->gc_work == NULL) {
        tc->gc_work_size = 16;
        tc->gc_work = malloc(tc->gc_work_size * sizeof(MVMWorkThread));
    }

    for (i = 0; i < tc->gc_work_count; i++) {
        MVMThreadContext *child = tc->gc_work[i].tc;

        if (MVM_try_steal(tc, child, tmp0)) {
            /* We successfully stole the child; update its
             * position in the work queue if necessary. */
            tc->gc_work[count++].tc = child;
        }
    }

    /* Add ourself. Reset the count to how many we actually stole. */
    tc->gc_work[count++].tc = tc;
    tc->gc_work_count = count;

    /* Increment the finish and ack counters to signify we're registering in
     * the GC run and for the other to wait for us to finish those two
     * phases, and notify the others we're ready to start. */
    MVM_incr(&tc->instance->gc_finish);
    MVM_incr(&tc->instance->gc_ack);
    MVM_decr(&tc->instance->gc_start);
	GCDEBUG_LOG(tc, MVM_GC_DEBUG_ORCHESTRATE, "decr gc_start to %d\n", MVM_load(&tc->instance->gc_start));
}

/* Does work in a thread's in-tray, if any. */
static void process_in_tray(MVMThreadContext *tc, MVMuint8 gen, MVMuint32 *put_vote) {
    /* Do we have any more work given by another thread? If so, re-enter
     * GC loop to process it. Note that since we're now doing GC stuff
     * again, we take back our vote to finish. */
    if (MVM_load(&tc->gc_in_tray)) {
        GCDEBUG_LOG(tc, MVM_GC_DEBUG_ORCHESTRATE, "Was given extra work by another thread; doing it\n");
        if (!*put_vote) {
            MVM_incr(&tc->instance->gc_finish);
            *put_vote = 1;
        }
        MVM_gc_collect(tc, MVMGCWhatToDo_InTray, gen);
    }
}

/* Checks whether our sent items have been completed. Returns 0 if all work is done. */
static MVMuint32 process_sent_items(MVMThreadContext *tc, MVMuint32 *put_vote) {
    /* Is any of our work outstanding? If so, take away our finish vote.
     * If we successfully check all our work, add the finish vote back. */
    MVMGCPassedWork *work = tc->gc_next_to_check;
    MVMuint32 advanced = 0;
    if (work) {
        /* if we have a submitted work item we haven't claimed a vote for, get a vote. */
        if (!*put_vote) {
            MVM_incr(&tc->instance->gc_finish);
            *put_vote = 1;
        }
        while (MVM_load(&work->completed)) {
            advanced = 1;
            work = work->next_by_sender;
            if (!work) break;
        }
        if (advanced)
            tc->gc_next_to_check = work;
        /* if all our submitted work items are completed, release the vote. */
        /* otherwise indicate that something we submitted isn't finished */
        return work ? (work->upvoted = 1) : 0;
    }
    return 0;
}

static void cleanup_sent_items(MVMThreadContext *tc) {
    MVMGCPassedWork *work, *next = tc->gc_sent_items;
    while ((work = next)) {
        next = work->last_by_sender;
        free(work);
    }
    tc->gc_sent_items = NULL;
}

/* Called by a thread when it thinks it is done with GC. It may get some more
 * work yet, though. */
static void finish_gc(MVMThreadContext *tc, MVMuint8 gen) {
    MVMuint32 put_vote = 1, i;

    /* Loop until other threads have terminated, processing any extra work
     * that we are given. */
    while (MVM_load(&tc->instance->gc_finish)) {
        MVMuint32 failed = 0;
        MVMuint32 i = 0;

        for ( ; i < tc->gc_work_count; i++) {
            MVMThreadContext *other = tc->gc_work[i].tc;
            other->gc_owner_tc = tc;
            process_in_tray(other, gen, &put_vote);
            failed |= process_sent_items(other, &put_vote);
            other->gc_owner_tc = NULL;
        }

        if (!failed && put_vote) {
            MVM_decr(&tc->instance->gc_finish);
            put_vote = 0;
        }
    }

    /* Reset GC status flags and cleanup sent items for any work threads. */
    /* This is also where thread destruction happens, and it needs to happen
     * before we acknowledge this GC run is finished. */
    for (i = 0; i < tc->gc_work_count; i++) {
        MVMThreadContext *other = tc->gc_work[i].tc;
        MVMThread *thread_obj = other->thread_obj;
        other->gc_owner_tc = tc;
        cleanup_sent_items(other);
        if (MVM_load(&thread_obj->body.stage) == MVM_thread_stage_clearing_nursery) {
            GCDEBUG_LOG(tc, MVM_GC_DEBUG_ORCHESTRATE, "freeing gen2 of thread %d\n", other->thread_id);
            /* always free gen2 */
            MVM_gc_collect_free_gen2_unmarked(other);
            GCDEBUG_LOG(tc, MVM_GC_DEBUG_ORCHESTRATE, "transferring gen2 of thread %d\n", other->thread_id);
            MVM_gc_gen2_transfer(other, tc);
            GCDEBUG_LOG(tc, MVM_GC_DEBUG_ORCHESTRATE, "destroying thread %d\n", other->thread_id);
            MVM_store(&thread_obj->body.stage, MVM_thread_stage_destroyed);
            MVM_tc_destroy(other);
            MVM_store(&thread_obj->body.tc, NULL);
            MVM_decr(&tc->instance->gc_morgue_thread_count);
            MVM_decr(&tc->instance->num_user_threads);
        }
        else {
            if (MVM_load(&thread_obj->body.stage) == MVM_thread_stage_exited) {
                /* don't bother freeing gen2; we'll do it next time */
                MVM_store(&thread_obj->body.stage, MVM_thread_stage_clearing_nursery);
                GCDEBUG_LOG(tc, MVM_GC_DEBUG_ORCHESTRATE, "discovered exited thread: set thread %d clearing nursery stage to %d\n", other->thread_id, MVM_load(&thread_obj->body.stage));
            }
            else {
                GCDEBUG_LOG(tc, MVM_GC_DEBUG_ORCHESTRATE, "thread %d stage was found to be %d\n", other->thread_id, MVM_load(&thread_obj->body.stage));
            }
            MVM_cas(&other->gc_status, MVMGCStatus_STOLEN, MVMGCStatus_UNABLE);
            MVM_cas(&other->gc_status, MVMGCStatus_INTERRUPT, MVMGCStatus_NONE);
        }
        other->gc_owner_tc = NULL;
    }
    /* Signal acknowledgement of completing the cleanup,
     * except for STables, and if we're the final to do
     * so, free the STables, which have been linked. */
    if (MVM_decr(&tc->instance->gc_ack) == 2) {
        /* Free any STables that have been marked for
         * deletion. It's okay for us to muck around in
         * another thread's fromspace while it's mutating
         * tospace, really. */
        MVM_gc_collect_free_stables(tc);

        /* Set it to zero (we're guaranteed the only ones
         * trying to write to it here). */
        MVM_store(&tc->instance->gc_ack, 0);
    }
}

void MVM_gc_run_gc(MVMThreadContext *tc, MVMuint8 what_to_do) {
    MVMuint8   gen;
    MVMThread *child;
    MVMuint32  i, n;

    /* Wait for all threads to indicate readiness to collect. */
    register_for_gc_run(tc);
    while (MVM_load(&tc->instance->gc_start));

    /* Do GC work for this thread, or at least all we know about. */
    gen = MVM_load(&tc->instance->gc_seq_number) % MVM_GC_GEN2_RATIO == 0
        ? MVMGCGenerations_Both
        : MVMGCGenerations_Nursery;

    /* Do GC work for any work threads. */
    for (i = 0, n = tc->gc_work_count ; i < n; i++) {
        MVMThreadContext *other = tc->gc_work[i].tc;
        other->gc_owner_tc = tc;
        tc->gc_work[i].limit = other->nursery_alloc;
        GCDEBUG_LOG(tc, MVM_GC_DEBUG_ORCHESTRATE,
            "thread %d starting collection for thread %d\n",
            tc->thread_id, other->thread_id);
        MVM_gc_collect(other, (other == tc ? what_to_do : MVMGCWhatToDo_NoInstance), gen);
        other->gc_owner_tc = NULL;
    }

    /* Wait for everybody to agree we're done. */
    finish_gc(tc, gen);

    /* Now we're all done, it's safe to finalize any objects that need it. */
	/* XXX TODO explore the feasability of doing this in a background
	 * finalizer/destructor thread and letting the main thread(s) continue
	 * on their merry way(s). */
    for (i = 0, n = tc->gc_work_count ; i < n; i++) {
        MVMThreadContext *other = tc->gc_work[i].tc;
        MVMThread *thread_obj = other->thread_obj;

        MVM_gc_collect_free_nursery_uncopied(other, tc->gc_work[i].limit);

        if (gen == MVMGCGenerations_Both) {
            GCDEBUG_LOG(tc, MVM_GC_DEBUG_ORCHESTRATE, "freeing gen2 of thread %d\n", other->thread_id);
            MVM_gc_collect_cleanup_gen2roots(other);
            MVM_gc_collect_free_gen2_unmarked(other);
        }
    }
    /* Reset the work count. */
    tc->gc_work_count = 0;
}
