#include "moarvm.h"

#define register_gc_run_thread_id(thread) \
    MVM_store(&(thread)->gc_thread_id, MVM_incr(&(thread)->instance->gc_thread_id) + 1)

/* This public-domain C quick sort implementation by Darel Rex Finley. */
/* for example, quicksort_maker(elements, 100, MVMint64, arr[L], arr[R],
 * MVMint64, arr[L], arr[R]); elem_type is the type of each element.
 * comparison_type is the type of the comparison value. */
#define quicksort_maker(elements, max_levels, \
        comparison_type, L_expr, R_expr, \
        elem_type, L_lvalue, R_lvalue) do { \
    elem_type piv_element; \
    comparison_type piv_value; \
    MVMint64 beg[max_levels], end[max_levels], i = 0, L, R ; \
    beg[0] = 0; \
    end[0] = elements; \
    while (i >= 0) { \
        L = beg[i]; \
        R = end[i] - 1; \
        if (L < R) { \
            piv_element = L_lvalue; \
            piv_value = L_expr; \
            if (i == max_levels - 1) \
                MVM_exception_throw_adhoc(tc, "quicksort overflow"); \
            while (L < R) { \
                while (R_expr >= piv_value && L < R) \
                    R--; \
                if (L < R) { \
                    L_lvalue = R_lvalue; \
                    L++; \
                } \
                while (L_expr <= piv_value && L < R) \
                    L++; \
                if (L < R) { \
                    R_lvalue = L_lvalue; \
                    R--; \
                } \
            } \
            L_lvalue = piv_element; \
            beg[i+1] = L + 1; \
            end[i+1] = end[i]; \
            end[i++] = L; \
        } \
        else { \
            i--; \
        } \
    } \
} while (0)

/* Processes the gc_work queue, attempting to steal the children.  They are
 * guaranteed to exist because every thread lasts at least 1 GC run, and the
 * entry stays in this table no more than 1 GC run. */
void MVM_gc_register_for_gc_run(MVMThreadContext *tc) {
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

    /* Increment the finish counter to signify we're registering in
     * the GC run and for the other to wait for us to finish that
     * phase. */
    MVM_incr(&tc->instance->gc_finish);
}

/* Takes work in a thread's in-tray, if any, and adds it to the worklist. */
static void add_in_tray_to_worklist(MVMThreadContext *tc, MVMGCWorklist *worklist) {
    MVMGCPassedWork * volatile *in_tray = &tc->gc_in_tray;
    MVMGCPassedWork *head;

    /* Get work to process. */
    while (1) {
        /* See if there's anything in the in-tray; if not, we're done. */
        head = (MVMGCPassedWork *)MVM_load(in_tray);
        if (head == NULL)
            return;

        /* Otherwise, try to take it. */
        if (MVM_casptr(in_tray, head, NULL) == head)
            break;
    }

    /* Go through list, adding to worklist. */
    while (head) {
        MVMGCPassedWork *next = (MVMGCPassedWork *)MVM_load(&head->next);
        MVMGCWorklist *head_worklist = (MVMGCWorklist *)head;
        MVM_gc_worklist_copy_items_to(tc, head_worklist, worklist, 0);

        /* Signal that we've consumed it; the sender will destroy it. */
        head_worklist->items = 0;
        GCDEBUG_LOG(tc, MVM_GC_DEBUG_PASSEDWORK,
            "completed work item %p\n", head);
        head = next;
    }
}

static void process_passed_work(MVMThreadContext *tc, MVMGCPassedWork *head,
        MVMGCWorklist **worklist_ptr, MVMuint8 gen) {
    MVMThreadContext *last_tc = NULL;
    MVMuint32 i, j;
    MVMGCWorklist *mainlist, *worklist = *worklist_ptr;

    GCDEBUG_LOG(tc, MVM_GC_DEBUG_ORCHESTRATE,
        "Was given extra work by another thread; doing it; %p\n", head);

    mainlist = &head->worklist;

    quicksort_maker(mainlist->items, 100, MVMThreadContext *,
        (*mainlist->list[L])->manager, (*mainlist->list[R])->manager,
        MVMCollectable **, mainlist->list[L], mainlist->list[R]);

    /* make a worklist if we didn't already */
    if (!worklist)
        *worklist_ptr = worklist = MVM_gc_worklist_create(tc);

    /* Process each of the blocks of items using its items' tc. */
    /* j is the start of the last block of items with the same tc. */
    for (i = 0, j = 0; i < mainlist->items; i++) {
        if ((*mainlist->list[i])->manager == last_tc)
            continue;

        if (last_tc) {
            /* reset the items in worklist so it appends freshly. */
            worklist->items = 0;
            MVM_gc_worklist_copy_items_to(tc, mainlist, worklist, j);
            MVM_gc_process_worklist(last_tc, worklist, gen);
        }
        last_tc = (*mainlist->list[i])->manager;
        j = i;
    }
    if (last_tc) {
        /* reset the items in worklist so it appends freshly. */
        worklist->items = 0;
        MVM_gc_worklist_copy_items_to(tc, mainlist, worklist, j);
        MVM_gc_process_worklist(last_tc, worklist, gen);
    }

    /* Cleanup and reset */
    mainlist->items = 0;
    GCDEBUG_LOG(tc, MVM_GC_DEBUG_PASSEDWORK,
        "completed work item %p\n", head);
}

/* Does work in a thread's in-tray, if any. */
void MVM_gc_process_in_tray(MVMThreadContext *tc, MVMuint8 gen, MVMuint32 *put_vote) {
    MVMGCPassedWork *head;
    MVMGCWorklist *worklist = NULL;

    /* Get work to process. */
    /* See if there's anything in the in-tray; if not, we're done. */
    while ((head = (MVMGCPassedWork *)MVM_load(&tc->gc_in_tray))) {

        /* Otherwise, try to take it. */
        if (MVM_casptr(&tc->gc_in_tray, head, NULL) != head)
            continue;

        if (!*put_vote) {
            MVM_incr(&tc->instance->gc_finish);
            *put_vote = 1;
        }

        /* Process each of the passed work items in the chain. */
        do {
            /* must read next first because the sender thread might free it. */
            MVMGCPassedWork *next = (MVMGCPassedWork *)MVM_load(&head->next);
            process_passed_work(tc, head, &worklist, gen);
            head = next;
        } while (head);
    }

    if (worklist) {
        /* Destroy the worklist. */
        MVM_gc_worklist_destroy(tc, worklist);
    }
}

/* Checks whether our sent items have been completed. */
static void verify_sent_items(MVMThreadContext *tc, MVMuint32 *put_vote) {
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
            MVMGCPassedWork *next_work = work->next_by_sender;
            advanced = 1;
            MVM_gc_worklist_destroy(tc, (MVMGCWorklist *)work);
            work = next_work;
            if (!next_work) break;
        }
        if (advanced) {
            tc->gc_next_to_check = work;
        }
    }
}

static void reset_gc_status(MVMThreadContext *tc) {
        GCDEBUG_LOG(tc, MVM_GC_DEBUG_ORCHESTRATE,
            "resetting gc_status which is %d\n", MVM_load(&tc->gc_status));
    if (MVM_load(&tc->gc_status) == MVMGCStatus_STOLEN) {
        if (!MVM_trycas(&tc->gc_status,
                MVMGCStatus_STOLEN, MVMGCStatus_UNABLE)) {
            MVM_panic(1, "gc failure");
        }
        else {
            GCDEBUG_LOG(tc, MVM_GC_DEBUG_ORCHESTRATE,
                "unstole thread (set to unable)\n");
        }
    }
    else if (!MVM_trycas(&tc->gc_status,
            MVMGCStatus_INTERRUPT, MVMGCStatus_NONE)) {
    }
}

/* Do GC work for any work threads. */
static void do_collections(MVMThreadContext *tc, MVMuint8 what_to_do, MVMuint8 gen) {
    MVMuint32  i;

    for (i = 0; i < tc->gc_work_count; i++) {
        MVMThreadContext *target = tc->gc_work[i].tc;
        GCDEBUG_LOG(target, MVM_GC_DEBUG_ORCHESTRATE, "starting collection\n");

        reset_gc_status(target);

        /* Set limit for later collection. */
        tc->gc_work[i].limit = target->nursery_alloc;

        /* Call the collector. We may call it again later for this thread. */
        MVM_gc_collect(target, (MVM_load(&target->gc_thread_id) != 0
            ? what_to_do : MVMGCWhatToDo_NoInstance), gen);
    }
}

static void collect_intrays(MVMThreadContext *tc, MVMuint8 gen) {
    MVMuint32 put_vote = 1;

    /* Loop until other threads have terminated, processing any extra work
     * that we are given. */
    /* Don't use an atomic load here since we're spinning in
     * this loop and I imagine the contention overhead far outweighs
     * any latency improvement from getting the zero value asap. */
    while (tc->instance->gc_finish) {
        do {
            MVM_gc_process_in_tray(tc, gen, &put_vote);
            verify_sent_items(tc, &put_vote);
        } while (tc->gc_next_to_check);

        if (put_vote) {
            MVM_decr(&tc->instance->gc_finish);
            put_vote = 0;
        }
    }
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

static void cleanup_gc_run(MVMThreadContext *tc, MVMuint8 gen) {
    MVMuint32 i;
    
    /* Reset GC status flags and cleanup sent items for any work threads. */
    /* This is also where thread destruction happens, and it needs to happen
     * before we acknowledge this GC run is finished. */
    for (i = 0; i < tc->gc_work_count; i++) {
        MVMThreadContext *target = tc->gc_work[i].tc;
        MVMThread *thread_obj = target->thread_obj;

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

    /* Reset the work count. */
    tc->gc_work_count = 0;
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
}

/* Now we're all done, it's safe to finalize any objects that need it. */
static void finalize_objects(MVMThreadContext *tc, MVMuint8 gen) {
    MVMuint32  i;

    for (i = 0; i < tc->gc_work_count; i++)
        post_run_frees_and_cleanups(tc->gc_work[i].tc,
            tc->gc_work[i].limit, gen);
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
        MVM_store(&tc->instance->gc_thread_id, 0);
        MVM_decr(&tc->instance->gc_ack);
    }
    GCDEBUG_LOG(tc, MVM_GC_DEBUG_ORCHESTRATE, "acknowledged finish\n");
}

/* Do GC work for this thread and stolen ones. */
void MVM_gc_run_gc(MVMThreadContext *tc, MVMuint8 what_to_do) {
    MVMuint8   gen;
    AO_t tmp0 = 0;

    /* Indicate readiness to collect. */
    tmp0 = MVM_decr(&tc->instance->gc_start);
    GCDEBUG_LOG(tc, MVM_GC_DEBUG_GCSTART, "decr gc_start to %d in run_gc\n", tmp0 - 1);

    /* Wait for all threads to indicate readiness to collect. */
    while (MVM_load(&tc->instance->gc_start));
    MVM_incr(&tc->instance->gc_ack);

    gen = MVM_load(&tc->instance->gc_seq_number) % MVM_GC_GEN2_RATIO == 0
        ? MVMGCGenerations_Both
        : MVMGCGenerations_Nursery;

    do_collections(tc, what_to_do, gen);

    /* Wait for everybody to agree we're done. */
    collect_intrays(tc, gen);
    finalize_objects(tc, gen);
    cleanup_gc_run(tc, gen);
    acknowledge_finish(tc);
}
