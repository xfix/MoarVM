#include "moar.h"

#ifdef _MSC_VER
#define do_worklist(tc, worklist, gen, func, name, ...) do { \
    func(tc, worklist, __VA_ARGS__); \
    GCDEBUG_LOG(tc, MVM_GC_DEBUG_COLLECT, \
        "processing %d items from " name "\n", worklist->items); \
    MVM_gc_process_worklist(tc, worklist, gen); \
} while (0)
#else
#define do_worklist(tc, worklist, gen, func, name, ...) do { \
    func(tc, worklist , ##__VA_ARGS__); \
    GCDEBUG_LOG(tc, MVM_GC_DEBUG_COLLECT, \
        "processing %d items from " name "\n", worklist->items); \
    MVM_gc_process_worklist(tc, worklist, gen); \
} while (0)
#endif

/* Forward decls. */
static void pass_work_item(MVMThreadContext *tc, MVMCollectable **item_ptr);
static void pass_leftover_work(MVMThreadContext *tc);

/* Does a garbage collection run. Exactly what it does is configured by the
 * couple of arguments that it takes.
 *
 * The what_to_do argument specifies where it should look for things to add
 * to the worklist: everywhere, just at thread local stuff, or just in the
 * thread's in-tray.
 *
 * The gen argument specifies whether to collect the nursery or both of the
 * generations. Nursery collection is done by semi-space copying. Once an
 * object is seen/copied once in the nursery (may be tuned in the future to
 * twice or so - we'll see) then it is not copied to tospace, but instead
 * promoted to the second generation. If we are collecting generation 2 also,
 * then objects that are alive in the second generation are simply marked.
 * Since the second generation is managed as a set of sized pools, there is
 * much less motivation for any kind of copying/compaction; the internal
 * fragmentation that makes finding a right-sized gap problematic will not
 * happen.
 *
 * Note that it adds the roots and processes them in phases, to try to avoid
 * building up a huge worklist. */
void MVM_gc_collect(MVMThreadContext *tc, MVMuint8 what_to_do, MVMuint8 gen) {
    /* Create a GC worklist. */
    MVMGCWorklist *worklist = MVM_gc_worklist_create(tc);

    /* Swap fromspace and tospace. */
    void * fromspace = tc->nursery_tospace;
    void * tospace   = tc->nursery_fromspace;
    tc->nursery_fromspace = fromspace;
    tc->nursery_tospace   = tospace;

    /* Reset nursery allocation pointers to the new tospace. */
    tc->nursery_alloc       = tospace;
    tc->nursery_alloc_limit = (char *)tc->nursery_alloc + MVM_NURSERY_SIZE;

    do_worklist(tc, worklist, gen,
        MVM_gc_worklist_add, "thread_obj", &tc->thread_obj);

    /* Add permanent roots and process them; only one thread will do
    * this, since they are instance-wide. */
    if (what_to_do != MVMGCWhatToDo_NoInstance) {
        do_worklist(tc, worklist, gen,
            MVM_gc_root_add_permanents_to_worklist, "instance permanents");
        do_worklist(tc, worklist, gen,
            MVM_gc_root_add_instance_roots_to_worklist, "instance roots");
    }

    /* Add per-thread state to worklist and process it. */
    do_worklist(tc, worklist, gen,
        MVM_gc_root_add_tc_roots_to_worklist, "TC objects");

    /* Add temporary roots and process them (these are per-thread). */
    do_worklist(tc, worklist, gen,
        MVM_gc_root_add_temps_to_worklist, "thread temps");

    /* Add things that are roots for the first generation because they are
    * pointed to by objects in the second generation and process them
    * (also per-thread). Note we need not do this if we're doing a full
    * collection anyway (in fact, we must not for correctness, otherwise
    * the gen2 rooting keeps them alive forever). */
    if (gen == MVMGCGenerations_Nursery)
        do_worklist(tc, worklist, gen,
            MVM_gc_root_add_gen2s_to_worklist, "gen2");

    /* Find roots in frames and process them. */
    if (tc->cur_frame)
        do_worklist(tc, worklist, gen,
            MVM_gc_worklist_add_frame, "cur_frame", tc->cur_frame);

    /* At this point, we have probably done most of the work we will
     * need to (only get more if another thread passes us more); zero
     * out the remaining tospace. */
    memset(tc->nursery_alloc, 0,
        (char *)tc->nursery_alloc_limit - (char *)tc->nursery_alloc);

    /* Destroy the worklist. */
    MVM_gc_worklist_destroy(tc, worklist);

    /* Pass any work for other threads we accumulated but that didn't trigger
     * the work passing threshold, then cleanup work passing list. */
    pass_leftover_work(tc);
}

/* Processes the current worklist. */
void MVM_gc_process_worklist(MVMThreadContext *tc, MVMGCWorklist *worklist, MVMuint8 gen) {
    MVMGen2Allocator  *gen2;
    MVMCollectable   **item_ptr;
    MVMCollectable    *new_addr;
    MVMuint32          gen2count;
    MVMuint16          i;

    /* Grab the second generation allocator; we may move items into the
     * old generation. */
    gen2 = tc->gen2;

    MVM_gc_worklist_mark_frame_roots(tc, worklist);

    while ((item_ptr = MVM_gc_worklist_get(tc, worklist))) {
        /* Dereference the object we're considering. */
        MVMCollectable *item = *item_ptr;
        MVMuint8 item_gen2;

        /* If the item is NULL, that's fine - it's just a null reference and
         * thus we've no object to consider. */
        if (item == NULL)
            continue;

        /* If it's in the second generation and we're only doing a nursery,
         * collection, we have nothing to do. */
        item_gen2 = item->flags & MVM_CF_SECOND_GEN;
        if (item_gen2 && gen == MVMGCGenerations_Nursery)
            continue;

        /* If the item was already seen and copied, then it will have a
         * forwarding address already. Just update this pointer to the
         * new address and we're done. */
        if (item->forwarder) {
            if (MVM_GC_DEBUG_ENABLED(MVM_GC_DEBUG_COLLECT)) {
                if (*item_ptr != item->forwarder) {
                    GCDEBUG_LOG(tc, MVM_GC_DEBUG_COLLECT,
                        "updating handle %p from %p to forwarder %p\n",
                        item_ptr, item, item->forwarder);
                }
                else {
                    GCDEBUG_LOG(tc, MVM_GC_DEBUG_COLLECT,
                        "already visited handle %p to forwarder %p\n",
                        item_ptr, item->forwarder);
                }
            }
            *item_ptr = item->forwarder;
            continue;
        }

        /* If the pointer is already into tospace (the bit we've already copied
         * into), we already updated it, so we're done. If it's in to-space but
         * *ahead* of our copy offset then it's an out-of-date pointer and we
         * have some kind of corruption. */
        if (item >= (MVMCollectable *)tc->nursery_tospace && item < (MVMCollectable *)tc->nursery_alloc)
            continue;
        if (item >= (MVMCollectable *)tc->nursery_alloc && item < (MVMCollectable *)tc->nursery_alloc_limit)
            MVM_panic(1, "Heap corruption detected: pointer %p to past fromspace", item);

        /* If it's owned by a different thread, we need to pass it over to
         * the owning thread. */
        if (item->manager != tc) {
            GCDEBUG_LOG(tc, MVM_GC_DEBUG_COLLECT,
                "sending a handle %p to object %p to thread %d\n",
                item_ptr, item, item->manager->thread_id);
            pass_work_item(tc, item_ptr);
            continue;
        }

        /* At this point, we didn't already see the object, which means we
         * need to take some action. Go on the generation... */
        if (item_gen2) {
            /* It's in the second generation. We'll just mark it, which is
             * done by setting the forwarding pointer to the object itself,
             * since we don't do moving. */
            new_addr = item;
            if (MVM_GC_DEBUG_ENABLED(MVM_GC_DEBUG_COLLECT)) {
                if (new_addr != item) {
                    GCDEBUG_LOG(tc, MVM_GC_DEBUG_COLLECT,
                        "updating handle %p from referent %p to %p\n",
                        item_ptr, item, new_addr);
                }
                else {
                    GCDEBUG_LOG(tc, MVM_GC_DEBUG_COLLECT,
                        "handle %p was already %p\n", item_ptr, new_addr);
                }
            }
            *item_ptr = item->forwarder = new_addr;
        } else {
            /* Catch NULL stable (always sign of trouble) in debug mode. */
            if (MVM_GC_DEBUG_ENABLED(MVM_GC_DEBUG_COLLECT) && !STABLE(item)) {
                GCDEBUG_LOG(tc, MVM_GC_DEBUG_COLLECT,
                    "found a zeroed handle %p to object %p\n", item_ptr, item);
                MVM_panic(1, "panic");
            }

            /* Did we see it in the nursery before? */
            if (item->flags & MVM_CF_NURSERY_SEEN) {
                /* Yes; we should move it to the second generation. Allocate
                 * space in the second generation. */
                new_addr = MVM_gc_gen2_allocate(gen2, item->size);

                /* Copy the object to the second generation and mark it as
                 * living there. */
                GCDEBUG_LOG(tc, MVM_GC_DEBUG_COLLECT,
                    "copying an object %p of size %d to gen2 %p\n",
                    item, item->size, new_addr);
                memcpy(new_addr, item, item->size);
                new_addr->flags ^= MVM_CF_NURSERY_SEEN;
                new_addr->flags |= MVM_CF_SECOND_GEN;

                /* If it references frames or static frames, we need to keep
                 * on visiting it. */
                if (!(new_addr->flags & (MVM_CF_TYPE_OBJECT | MVM_CF_STABLE))) {
                    MVMObject *new_obj_addr = (MVMObject *)new_addr;
                    if (REPR(new_obj_addr)->refs_frames)
                        MVM_gc_root_gen2_add(tc, (MVMCollectable *)new_obj_addr);
                }

                /* If we're going to sweep the second generation, also need
                 * to mark it as live. */
                if (gen == MVMGCGenerations_Both)
                    new_addr->forwarder = new_addr;
            }
            else {
                /* No, so it will live in the nursery for another GC
                 * iteration. Allocate space in the nursery. */
                new_addr = (MVMCollectable *)tc->nursery_alloc;
                tc->nursery_alloc = (char *)tc->nursery_alloc + item->size;
                GCDEBUG_LOG(tc, MVM_GC_DEBUG_COLLECT,
                    "copying an object %p of size %d to tospace %p\n",
                    item, item->size, new_addr);

                /* Copy the object to tospace and mark it as seen in the
                 * nursery (so the next time around it will move to the
                 * older generation, if it survives). */
                memcpy(new_addr, item, item->size);
                new_addr->flags |= MVM_CF_NURSERY_SEEN;
            }

            /* Store the forwarding pointer and update the original
             * reference. */
            if (MVM_GC_DEBUG_ENABLED(MVM_GC_DEBUG_COLLECT) && new_addr != item) {
                GCDEBUG_LOG(tc, MVM_GC_DEBUG_COLLECT,
                    "updating handle %p from referent %p to %p\n",
                    item_ptr, item, new_addr);
            }
            *item_ptr = item->forwarder = new_addr;
        }

        /* make sure any rooted frames mark their stuff */
        MVM_gc_worklist_mark_frame_roots(tc, worklist);

        /* Finally, we need to mark the collectable (at its moved address).
         * Track how many items we had before we mark it, in case we need
         * to write barrier them post-move to uphold the generational
         * invariant. */
        gen2count = worklist->items;
        MVM_gc_mark_collectable(tc, worklist, new_addr);

        /* make sure any rooted frames mark their stuff */
        MVM_gc_worklist_mark_frame_roots(tc, worklist);

        /* In moving an object to generation 2, we may have left it pointing
         * to nursery objects. If so, make sure it's in the gen2 roots. */
        if (new_addr->flags & MVM_CF_SECOND_GEN) {
            MVMCollectable **j;
            MVMuint32 max = worklist->items, k;

            for (k = gen2count; k < max; k++) {
                j = worklist->list[k];
                if (*j)
                    MVM_WB(tc, new_addr, *j);
            }
        }
    }
}

/* Marks a collectable item (object, type object, STable). */
void MVM_gc_mark_collectable(MVMThreadContext *tc, MVMGCWorklist *worklist, MVMCollectable *new_addr) {
    MVMuint16 i;

    MVM_gc_worklist_add(tc, worklist, &new_addr->sc);

    if (!(new_addr->flags & (MVM_CF_TYPE_OBJECT | MVM_CF_STABLE))) {
        /* Need to view it as an object in here. */
        MVMObject *new_addr_obj = (MVMObject *)new_addr;

        /* Add the STable to the worklist. */
        MVM_gc_worklist_add(tc, worklist, &new_addr_obj->st);

        /* If needed, mark it. This will add addresses to the worklist
         * that will need updating. Note that we are passing the address
         * of the object *after* copying it since those are the addresses
         * we care about updating; the old chunk of memory is now dead! */
        if (MVM_GC_DEBUG_ENABLED(MVM_GC_DEBUG_COLLECT) && !STABLE(new_addr_obj))
            MVM_panic(MVM_exitcode_gcnursery,
                "Found an outdated reference to address %p", new_addr);
        if (REPR(new_addr_obj)->gc_mark)
            REPR(new_addr_obj)->gc_mark(tc, STABLE(new_addr_obj),
                OBJECT_BODY(new_addr_obj), worklist);
    }
    else if (new_addr->flags & MVM_CF_TYPE_OBJECT) {
        /* Add the STable to the worklist. */
        MVM_gc_worklist_add(tc, worklist, &((MVMObject *)new_addr)->st);
    }
    else if (new_addr->flags & MVM_CF_STABLE) {
        /* Add all references in the STable to the work list. */
        MVMSTable *new_addr_st = (MVMSTable *)new_addr;
        MVM_gc_worklist_add(tc, worklist, &new_addr_st->HOW);
        MVM_gc_worklist_add(tc, worklist, &new_addr_st->WHAT);
        MVM_gc_worklist_add(tc, worklist, &new_addr_st->method_cache);
        for (i = 0; i < new_addr_st->vtable_length; i++)
            MVM_gc_worklist_add(tc, worklist, &new_addr_st->vtable[i]);
        for (i = 0; i < new_addr_st->type_check_cache_length; i++)
            MVM_gc_worklist_add(tc, worklist, &new_addr_st->type_check_cache[i]);
        if (new_addr_st->container_spec)
            new_addr_st->container_spec->gc_mark_data(tc, new_addr_st, worklist);
        if (new_addr_st->boolification_spec)
            MVM_gc_worklist_add(tc, worklist, &new_addr_st->boolification_spec->method);
        if (new_addr_st->invocation_spec) {
            MVM_gc_worklist_add(tc, worklist, &new_addr_st->invocation_spec->class_handle);
            MVM_gc_worklist_add(tc, worklist, &new_addr_st->invocation_spec->attr_name);
            MVM_gc_worklist_add(tc, worklist, &new_addr_st->invocation_spec->invocation_handler);
        }
        MVM_gc_worklist_add(tc, worklist, &new_addr_st->WHO);

        /* If it needs to have its REPR data marked, do that. */
        if (new_addr_st->REPR->gc_mark_repr_data)
            new_addr_st->REPR->gc_mark_repr_data(tc, new_addr_st, worklist);
    }
    else {
        MVM_panic(MVM_exitcode_gcnursery,
            "Internal error: impossible case encountered in GC marking");
    }
}

/* Adds a chunk of work to another thread's in-tray. */
static void push_work_to_thread_in_tray(MVMThreadContext *tc, MVMThreadContext *target, MVMGCPassedWork *work) {
    MVMint32 j;
    MVMThreadContext *orig_tc = tc;

    /* Become our owner again for a while... */
    tc = tc->gc_owner_tc;

    /* queue it up to check if the check list isn't clear */
    if (!tc->gc_next_to_check) {
        tc->gc_next_to_check = work;
    }

    /* Pass the work, chaining any other in-tray entries for the thread
     * after us. */
    while (1) {
        MVMGCPassedWork *orig = (MVMGCPassedWork *)MVM_load(&target->gc_in_tray);

        MVM_store(&work->next, orig);
        if (MVM_casptr(&target->gc_in_tray, orig, work) == orig) {
            GCDEBUG_LOG(orig_tc, MVM_GC_DEBUG_PASSEDWORK,
                "passed a work item %p to manager thread %d for thread %d\n",
                work, target->thread_id, (*work->worklist.list[0])->manager->thread_id);
            return;
        }
    }
}

/* Adds work to list of items to pass over to another thread, and if we
 * reach the pass threshold then does the passing. */
static void pass_work_item(MVMThreadContext *tc, MVMCollectable **item_ptr) {
    MVMInstance *i          = tc->instance;
    MVMThreadContext *target =
        (MVMThreadContext *)MVM_load(&(*item_ptr)->manager->gc_owner_tc);
    MVMGCPassedWork *wtp = NULL;

    /* Find any existing thread work passing list for the target. */
    if (!tc->gc_wtp) {
        MVMuint32 count = tc->instance->gc_thread_id > 16
            ? tc->instance->gc_thread_id : 16;
        tc->gc_wtp = calloc(count, sizeof(MVMGCPassedWork *));
        tc->gc_wtp_size = count;
    }
    else if (tc->gc_wtp_size < tc->instance->gc_thread_id) {
        MVMuint32 orig_size = tc->gc_wtp_size;
        do {
            tc->gc_wtp_size *= 2;
        } while (tc->gc_wtp_size < tc->instance->gc_thread_id);
        tc->gc_wtp = realloc(tc->gc_wtp, tc->gc_wtp_size * sizeof(MVMGCPassedWork *));
        memset(tc->gc_wtp + orig_size, 0,
            (tc->gc_wtp_size - orig_size) * sizeof(MVMGCPassedWork *));
    }

    /* If there's no entry for this target, create one. */
    if ((wtp = tc->gc_wtp[target->gc_thread_id]) == NULL)
        wtp = tc->gc_wtp[target->gc_thread_id] = MVM_gc_wtp_create(tc);

    /* Add this item to the work list. */
    MVM_gc_worklist_add_slow(tc, &wtp->worklist, item_ptr);

    /* If we've hit the limit, pass this work to the target thread. */
    if (wtp->worklist.items == MVM_GC_PASS_WORK_SIZE) {
        push_work_to_thread_in_tray(tc, (*item_ptr)->manager->gc_owner_tc, wtp);
        tc->gc_wtp[target->gc_thread_id] = NULL;
    }
}

/* Passes all work for other threads that we've got left in our to-pass list. */
static void pass_leftover_work(MVMThreadContext *tc) {
    MVMuint32 j;
    for (j = 0; j < tc->instance->gc_thread_id; j++)
        if (tc->gc_wtp && tc->gc_wtp[j]) {
            push_work_to_thread_in_tray(tc,
                (MVMThreadContext *)MVM_load(&(*tc->gc_wtp[j]->worklist.list[0])->manager->gc_owner_tc),
                tc->gc_wtp[j]);
            tc->gc_wtp[j] = NULL;
        }
}

/* Save dead STable pointers to delete later.. */
static void MVM_gc_collect_enqueue_stable_for_deletion(MVMThreadContext *tc, MVMSTable *st) {
    MVMSTable *old_head;
    do {
        old_head = (MVMSTable *)MVM_load(&tc->instance->stables_to_free);
		/* Borrow the forwarder slot ... */
        st->header.forwarder = (MVMCollectable *)old_head;
    } while (!MVM_trycas(&tc->instance->stables_to_free, old_head, st));
}

/* Some objects, having been copied, need no further attention. Others
 * need to do some additional freeing, however. This goes through the
 * fromspace and does any needed work to free uncopied things (this may
 * run in parallel with the mutator, which will be operating on tospace). */
void MVM_gc_collect_free_nursery_uncopied(MVMThreadContext *tc, void *limit) {
    /* We start scanning the fromspace, and keep going until we hit
     * the end of the area allocated in it. */
    void *scan = tc->nursery_fromspace;
    while (scan < limit) {
        /* The object here is dead if it never got a forwarding pointer
         * written in to it. */
        MVMCollectable *item = (MVMCollectable *)scan;
        MVMuint8 dead = item->forwarder == NULL;

        /* Now go by collectable type. */
        if (!(item->flags & (MVM_CF_TYPE_OBJECT | MVM_CF_STABLE))) {
            /* Object instance. If dead, call gc_free if needed. Scan is
             * incremented by object size. */
            MVMObject *obj = (MVMObject *)item;
            GCDEBUG_LOG(tc, MVM_GC_DEBUG_COLLECT,
                "collecting an object %d in the nursery with reprid %d\n",
                item, REPR(obj)->ID);
            if (dead && REPR(obj)->gc_free)
                REPR(obj)->gc_free(tc, obj);
        }
        else if (item->flags & MVM_CF_TYPE_OBJECT) {
            /* Type object; doesn't have anything extra that needs freeing. */
        }
        else if (item->flags & MVM_CF_STABLE) {
            MVMSTable *st = (MVMSTable *)item;
            if (dead) {
            GCDEBUG_LOG(tc, MVM_GC_DEBUG_COLLECT,
                "enqueuing an STable %d in the nursery to be freed\n", item);
                MVM_gc_collect_enqueue_stable_for_deletion(tc, st);
            }
        }
        else {
            printf("item flags: %d\n", item->flags);
            MVM_panic(MVM_exitcode_gcnursery,
                "Internal error: impossible case encountered in GC free");
        }

        /* Go to the next item. */
        scan = (char *)scan + item->size;
    }
}

/* Goes through the inter-generational roots and removes any that have been
* determined dead. Should run just after gen2 GC has run but before building
* the free list (which clears the marks). */
void MVM_gc_collect_cleanup_gen2roots(MVMThreadContext *tc) {
    MVMCollectable **gen2roots = tc->gen2roots;
    MVMuint32        num_roots = tc->num_gen2roots;
    MVMuint32        ins_pos   = 0;
    MVMuint32        i;
    for (i = 0; i < num_roots; i++)
        if (gen2roots[i]->forwarder)
            gen2roots[ins_pos++] = gen2roots[i];
    tc->num_gen2roots = ins_pos;
}

/* Free STables (in any thread/generation!) queued to be freed. */
void MVM_gc_collect_free_stables(MVMThreadContext *tc) {
    MVMSTable *st = (MVMSTable *)MVM_load(&tc->instance->stables_to_free);
    while (st) {
        MVMSTable *st_to_free = st;
        st = (MVMSTable *)st_to_free->header.forwarder;
        st_to_free->header.forwarder = NULL;
        MVM_6model_stable_gc_free(tc, st_to_free);
    }
    MVM_store(&tc->instance->stables_to_free, NULL);
}

/* Goes through the unmarked objects in the second generation heap and builds
 * free lists out of them. Also does any required finalization. */
void MVM_gc_collect_free_gen2_unmarked(MVMThreadContext *tc) {
    /* Visit each of the size class bins. */
    MVMGen2Allocator *gen2 = tc->gen2;
    MVMuint32 bin, obj_size, page;
    char ***freelist_insert_pos;
    for (bin = 0; bin < MVM_GEN2_BINS; bin++) {
        /* If we've nothing allocated in this size class, skip it. */
        if (gen2->size_classes[bin].pages == NULL)
            continue;

        /* Calculate object size for this bin. */
        obj_size = (bin + 1) << MVM_GEN2_BIN_BITS;

        /* freelist_insert_pos is a pointer to a memory location that
         * stores the address of the last traversed free list node (char **). */
        /* Initialize freelist insertion position to free list head. */
        freelist_insert_pos = &gen2->size_classes[bin].free_list;

        /* Visit each page. */
        for (page = 0; page < gen2->size_classes[bin].num_pages; page++) {
            /* Visit all the objects, looking for dead ones and reset the
             * mark for each of them. */
            char *cur_ptr = gen2->size_classes[bin].pages[page];
            char *end_ptr = page + 1 == gen2->size_classes[bin].num_pages
                ? gen2->size_classes[bin].alloc_pos
                : cur_ptr + obj_size * MVM_GEN2_PAGE_ITEMS;
            char **last_insert_pos = NULL;
            while (cur_ptr < end_ptr) {
                MVMCollectable *col = (MVMCollectable *)cur_ptr;

                /* Is this already a free list slot? If so, it becomes the
                 * new free list insert position. */
                if (*freelist_insert_pos == (char **)cur_ptr) {
                    freelist_insert_pos = (char ***)cur_ptr;
                }

                /* Otherwise, it must be a collectable of some kind. Is it
                 * live? */
                else if (col->forwarder) {
                    /* Yes; clear the mark. */
                    col->forwarder = NULL;
                }
                else {
                    GCDEBUG_LOG(tc, MVM_GC_DEBUG_COLLECT,
                        "collecting an object %p in the gen2\n", col);
                    /* No, it's dead. Do any cleanup. */
                    if (!(col->flags & (MVM_CF_TYPE_OBJECT | MVM_CF_STABLE))) {
                        /* Object instance; call gc_free if needed. */
                        MVMObject *obj = (MVMObject *)col;
                        if (REPR(obj)->gc_free)
                            REPR(obj)->gc_free(tc, obj);
                    }
                    else if (col->flags & MVM_CF_TYPE_OBJECT) {
                        /* Type object; doesn't have anything extra that needs freeing. */
                    }
                    else if (col->flags & MVM_CF_STABLE) {
                        if (col->sc == (MVMSerializationContext *)1) {
                            /* We marked it dead last time, kill it. */
                            MVM_6model_stable_gc_free(tc, (MVMSTable *)col);
                        }
                        else {
                            if (MVM_load(&tc->gc_status) == MVMGCStatus_NONE) {
                                /* We're in global destruction, so enqueue to the end
                                 * like we do in the nursery */
                                MVM_gc_collect_enqueue_stable_for_deletion(tc,
                                    (MVMSTable *)col);
                            } else {
                                /* There will definitely be another gc run,
                                 * so mark it as "died last time". */
                                col->sc = (MVMSerializationContext *)1;
                            }
                            /* Skip the freelist updating. */
                            continue;
                        }
                    }
                    else {
                        printf("item flags: %d\n", col->flags);
                        MVM_panic(MVM_exitcode_gcnursery,
                            "Internal error: impossible case encountered"
                            "in gen2 GC free");
                    }

                    /* Chain in to the free list. */
                    *((char **)cur_ptr) = (char *)*freelist_insert_pos;
                    *freelist_insert_pos = (char **)cur_ptr;

                    /* Update the pointer to the insert position to point to us */
                    freelist_insert_pos = (char ***)cur_ptr;
                }

                /* Move to the next object. */
                cur_ptr += obj_size;
            }
        }
    }
}

/* Allocates a new GC work to pass item. */
MVMGCPassedWork * MVM_gc_wtp_create(MVMThreadContext *tc) {
    MVMGCPassedWork *wtp = calloc(1, sizeof(MVMGCPassedWork));
    MVM_gc_worklist_init(tc, (MVMGCWorklist *)wtp);
    MVM_store(&wtp->next, NULL);
    return wtp;
}
