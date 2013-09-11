#include "moarvm.h"

#define register_gc_run_thread_id(thread) \
    MVM_store(&(thread)->gc_thread_id, MVM_incr(&(thread)->instance->gc_thread_id) + 1)

static void prepare_wtp_table(MVMThreadContext *tc) {
    MVMuint32 num_threads = MVM_load(&tc->instance->gc_thread_id);

    if (num_threads > 1 && tc->gc_wtp_size < num_threads) {
        MVMGCWorklist **wtp = malloc(num_threads * sizeof(MVMGCWorklist *));
        MVM_checked_free_null(tc->gc_wtp);
        tc->gc_wtp = wtp;
    }
}

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
            GCDEBUG_LOG(tc, MVM_GC_DEBUG_ORCHESTRATE,
                "stole the gc work of child thread %d\n", child->thread_id);
            /* We successfully stole the child; update its
             * position in the work queue if necessary. */
            tc->gc_work[count++].tc = child;
            /* Mark it as such. */
            MVM_store(&child->gc_owner_tc, tc);
            /* Register its GC thread ID. */
            register_gc_run_thread_id(child);
        }
    }
    /* Erase any stray pointers. */
    for (i = count; i < tc->gc_work_count; i++) {
        tc->gc_work[i].tc = NULL;
        tc->gc_work[i].limit = NULL;
    }

    /* Add ourself. Reset the count to how many we actually stole. */
    tc->gc_work[count++].tc = tc;
    /* Mark our ownership of ... ourself. */
    MVM_store(&tc->gc_owner_tc, tc);
    tc->gc_work_count = count;
    /* Register for a GC thread ID. */
    register_gc_run_thread_id(tc);
    prepare_wtp_table(tc);

    /* Increment the finish and ack counters to signify we're registering in
     * the GC run and for the other to wait for us to finish those two
     * phases, and notify the others we're ready to start. */
    MVM_incr(&tc->instance->gc_finish);
    MVM_incr(&tc->instance->gc_ack);
    /* Indicate readiness to collect. */
    tmp0 = MVM_decr(&tc->instance->gc_start);
    GCDEBUG_LOG(tc, MVM_GC_DEBUG_GCSTART, "decr gc_start to %d\n", tmp0 - 1);

    /* Wait for all threads to indicate readiness to collect. */
    while (MVM_load(&tc->instance->gc_start));
}

/* Does work in a thread's in-tray, if any. */
static void process_in_tray(MVMThreadContext *tc, MVMuint8 gen, MVMuint32 *put_vote) {
    /* Do we have any more work given by another thread? If so, re-enter
     * GC loop to process it. Note that since we're now doing GC stuff
     * again, we take back our vote to finish. */
    if (MVM_load(&tc->gc_in_tray)) {
        GCDEBUG_LOG(tc, MVM_GC_DEBUG_ORCHESTRATE,
            "Was given extra work by another thread; doing it\n");
        if (!*put_vote) {
            MVM_incr(&tc->instance->gc_finish);
            *put_vote = 1;
        }
        MVM_gc_collect(tc, MVMGCWhatToDo_InTray, gen);
    }
}

/* Checks whether our sent items have been completed. Returns 0 if all work is done. */
static MVMuint32 verify_sent_items(MVMThreadContext *tc, MVMuint32 *put_vote) {
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
        while (work->worklist.items == 0) {
            advanced = 1;
            work = work->next_by_sender;
            MVM_gc_worklist_destroy(tc, (MVMGCWorklist *)work);
            if (!work) break;
        }
        if (advanced)
            tc->gc_next_to_check = work;
        /* if all our submitted work items are completed, release the vote. */
        /* otherwise indicate that something we submitted isn't finished */
        return work ? 1 : 0;
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

static void reset_gc_status(MVMThreadContext *tc) {
    if (MVM_load(&tc->gc_status) == MVMGCStatus_STOLEN) {
        if (!MVM_trycas(&tc->gc_status,
                MVMGCStatus_STOLEN, MVMGCStatus_UNABLE))
            MVM_panic(1, "gc failure 177");
    }
    else if (!MVM_trycas(&tc->gc_status,
            MVMGCStatus_INTERRUPT, MVMGCStatus_NONE))
        MVM_panic(1, "gc failure 177");
}

static void finalize_gc_run(MVMThreadContext *tc) {
    MVMThread *thread_obj = tc->thread_obj;

    if (MVM_load(&thread_obj->body.stage) == MVM_thread_stage_exited) {
        GCDEBUG_LOG(tc, MVM_GC_DEBUG_ORCHESTRATE,
            "discovered this thread had exited\n");
        /* don't bother freeing gen2; we'll do it next time */
        MVM_store(&thread_obj->body.stage,
            MVM_thread_stage_clearing_nursery);
    }
    reset_gc_status(tc);
    MVM_store(&tc->gc_owner_tc, NULL);
    MVM_store(&tc->gc_thread_id, 0);
}

/* Destroy a (non-main) thread. Only called by the GC at a very specific time. */
void thread_destroy(MVMThreadContext *tc, MVMThreadContext *target) {
    MVMThread *thread_obj = target->thread_obj;

    GCDEBUG_LOG(tc, MVM_GC_DEBUG_ORCHESTRATE,
        "destroying thread %d\n", target->thread_id);
    MVM_gc_collect_free_gen2_unmarked(target);
    MVM_gc_gen2_transfer(target, tc);
    MVM_store(&thread_obj->body.stage, MVM_thread_stage_destroyed);
    MVM_store(&thread_obj->body.tc, NULL);
    MVM_decr(&tc->instance->gc_morgue_thread_count);
    MVM_decr(&tc->instance->num_user_threads);
    MVM_tc_destroy(target);
}

static void collect_intrays(MVMThreadContext *tc, MVMuint8 gen) {
    MVMuint32 put_vote = 1;

    /* Loop until other threads have terminated, processing any extra work
     * that we are given. */
    while (MVM_load(&tc->instance->gc_finish)) {
        MVMuint32 failed = 0;
        MVMuint32 i = 0;

        for ( ; i < tc->gc_work_count; i++) {
            MVMThreadContext *target = tc->gc_work[i].tc;
            process_in_tray(target, gen, &put_vote);
            failed |= verify_sent_items(target, &put_vote);
        }

        if (!failed && put_vote) {
            MVM_decr(&tc->instance->gc_finish);
            put_vote = 0;
        }
    }
}

static void acknowledge_finish(MVMThreadContext *tc) {
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

static void cleanup_gc_run(MVMThreadContext *tc, MVMuint8 gen) {
    MVMuint32 i;

    /* Reset GC status flags and cleanup sent items for any work threads. */
    /* This is also where thread destruction happens, and it needs to happen
     * before we acknowledge this GC run is finished. */
    for (i = 0; i < tc->gc_work_count; i++) {
        MVMThreadContext *target = tc->gc_work[i].tc;
        MVMThread *thread_obj = target->thread_obj;

        cleanup_sent_items(target);

        if (MVM_load(&thread_obj->body.stage)
                == MVM_thread_stage_clearing_nursery) {
            thread_destroy(tc, target);
        }
        else {
            finalize_gc_run(target);
        }
        /* Clean up our ownership links. */
        tc->gc_work[i].tc = NULL;
        tc->gc_work[i].limit = NULL;
    }
}

/* XXX TODO explore the feasability of doing this in a background
 * finalizer/destructor thread and letting the main thread(s) continue
 * on their merry way(s). */
static void post_run_frees_and_cleanups(MVMThreadContext *tc, void *limit, MVMuint8 gen) {
    MVMThread *thread_obj = tc->thread_obj;

    MVM_gc_collect_free_nursery_uncopied(tc, limit);

    if (gen == MVMGCGenerations_Both) {
        GCDEBUG_LOG(tc, MVM_GC_DEBUG_ORCHESTRATE, "freeing gen2\n");
        MVM_gc_collect_cleanup_gen2roots(tc);
        MVM_gc_collect_free_gen2_unmarked(tc);
        /* If it was a full collection, some of the things in gen2 that we root
         * due to point to gen1 objects may be dead. */
        MVM_gc_root_gen2_cleanup(tc);
    }

    /* Reset this for next time. */
    if (tc->gc_thread_id == 0)
        MVM_store(&tc->instance->gc_thread_id, 0);
}

/* Now we're all done, it's safe to finalize any objects that need it. */
static void finalize_objects(MVMThreadContext *tc, MVMuint8 gen) {
    MVMuint32  i;

    for (i = 0; i < tc->gc_work_count; i++)
        post_run_frees_and_cleanups(tc->gc_work[i].tc,
            tc->gc_work[i].limit, gen);
}

/* Do GC work for any work threads. */
static void do_collections(MVMThreadContext *tc, MVMuint8 what_to_do, MVMuint8 gen) {
    MVMuint32  i;

    for (i = 0; i < tc->gc_work_count; i++) {
        MVMThreadContext *target = tc->gc_work[i].tc;
        GCDEBUG_LOG(target, MVM_GC_DEBUG_ORCHESTRATE, "starting collection\n");

        /* Set limit for later collection. */
        tc->gc_work[i].limit = target->nursery_alloc;

        /* Call the collector. We may call it again later for this thread. */
        MVM_gc_collect(target, (MVM_load(&target->gc_thread_id) != 0
            ? what_to_do : MVMGCWhatToDo_NoInstance), gen);
    }
}

/* Do GC work for this thread and stolen ones. */
void MVM_gc_run_gc(MVMThreadContext *tc, MVMuint8 what_to_do) {
    MVMuint8   gen;

    register_for_gc_run(tc);

    gen = MVM_load(&tc->instance->gc_seq_number) % MVM_GC_GEN2_RATIO == 0
        ? MVMGCGenerations_Both
        : MVMGCGenerations_Nursery;

    do_collections(tc, what_to_do, gen);

    /* Wait for everybody to agree we're done. */
    collect_intrays(tc, gen);
    finalize_objects(tc, gen);
    cleanup_gc_run(tc, gen);
    acknowledge_finish(tc);

    /* Reset the work count. */
    tc->gc_work_count = 0;
}
