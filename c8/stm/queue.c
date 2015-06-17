
typedef struct queue_entry_s {
    object_t *object;
    struct queue_entry_s *next;
} queue_entry_t;

typedef union stm_queue_segment_u {
    struct {
        /* a chained list of fresh entries that have been allocated and
           added to this queue during the current transaction.  If the
           transaction commits, these are moved to 'old_entries'. */
        queue_entry_t *added_in_this_transaction;

        /* a point inside the chained list above such that all items from
           this point are known to contain non-young objects, for GC */
        queue_entry_t *added_young_limit;

        /* a chained list of old entries that the current transaction
           popped.  only used if the transaction is not inevitable:
           if it aborts, these entries are added back to 'old_entries'. */
        queue_entry_t *old_objects_popped;

        /* a queue is active when either of the two chained lists
           above is not empty, until the transaction commits.  (this
           notion is per segment.)  this flag says that the queue is
           already in the tree STM_PSEGMENT->active_queues. */
        bool active;
    };
    char pad[64];
} stm_queue_segment_t;


struct stm_queue_s {
    /* this structure is always allocated on a multiple of 64 bytes,
       and the 'segs' is an array of items 64 bytes each */
    stm_queue_segment_t segs[STM_NB_SEGMENTS];

    /* a chained list of old entries in the queue */
    queue_entry_t *volatile old_entries;
};


stm_queue_t *stm_queue_create(void)
{
    void *mem;
    int result = posix_memalign(&mem, 64, sizeof(stm_queue_t));
    assert(result == 0);
    memset(mem, 0, sizeof(stm_queue_t));
    return (stm_queue_t *)mem;
}

static void queue_free_entries(queue_entry_t *lst)
{
    while (lst != NULL) {
        queue_entry_t *next = lst->next;
        free(lst);
        lst = next;
    }
}

void stm_queue_free(stm_queue_t *queue)
{
    long i;
    dprintf(("free queue %p\n", queue));
    for (i = 0; i < STM_NB_SEGMENTS; i++) {
        stm_queue_segment_t *seg = &queue->segs[i];

        /* it is possible that queues_deactivate_all() runs in parallel,
           but it should not be possible at this point for another thread
           to change 'active' from false to true.  if it is false, then
           that's it */
        if (!seg->active) {
            assert(!seg->added_in_this_transaction);
            assert(!seg->added_young_limit);
            assert(!seg->old_objects_popped);
            continue;
        }

        struct stm_priv_segment_info_s *pseg = get_priv_segment(i + 1);
        spinlock_acquire(pseg->active_queues_lock);

        if (seg->active) {
            assert(pseg->active_queues != NULL);
            bool ok = tree_delete_item(pseg->active_queues, (uintptr_t)queue);
            assert(ok);
        }
        queue_free_entries(seg->added_in_this_transaction);
        queue_free_entries(seg->old_objects_popped);

        spinlock_release(pseg->active_queues_lock);
    }
    free(queue);
}

static inline void queue_lock_acquire(void)
{
    int num = STM_SEGMENT->segment_num;
    spinlock_acquire(get_priv_segment(num)->active_queues_lock);
}
static inline void queue_lock_release(void)
{
    int num = STM_SEGMENT->segment_num;
    spinlock_release(get_priv_segment(num)->active_queues_lock);
}

static void queue_activate(stm_queue_t *queue)
{
    stm_queue_segment_t *seg = &queue->segs[STM_SEGMENT->segment_num - 1];

    if (!seg->active) {
        queue_lock_acquire();
        if (STM_PSEGMENT->active_queues == NULL)
            STM_PSEGMENT->active_queues = tree_create();
        tree_insert(STM_PSEGMENT->active_queues, (uintptr_t)queue, 0);
        assert(!seg->active);
        seg->active = true;
        dprintf(("activated queue %p\n", queue));
        queue_lock_release();
    }
}

static void queues_deactivate_all(bool at_commit)
{
    queue_lock_acquire();

    bool added_any_old_entries = false;
    wlog_t *item;
    TREE_LOOP_FORWARD(STM_PSEGMENT->active_queues, item) {
        stm_queue_t *queue = (stm_queue_t *)item->addr;
        stm_queue_segment_t *seg = &queue->segs[STM_SEGMENT->segment_num - 1];
        queue_entry_t *head, *freehead;

        if (at_commit) {
            head = seg->added_in_this_transaction;
            freehead = seg->old_objects_popped;
        }
        else {
            head = seg->old_objects_popped;
            freehead = seg->added_in_this_transaction;
        }

        /* forget the two lists of entries */
        seg->added_in_this_transaction = NULL;
        seg->added_young_limit = NULL;
        seg->old_objects_popped = NULL;

        /* free the list of entries that must disappear */
        queue_free_entries(freehead);

        /* move the list of entries that must survive to 'old_entries' */
        if (head != NULL) {
            queue_entry_t *old;
            queue_entry_t *tail = head;
            while (tail->next != NULL)
                tail = tail->next;
            dprintf(("items move to old_entries in queue %p\n", queue));
         retry:
            old = queue->old_entries;
            tail->next = old;
            if (!__sync_bool_compare_and_swap(&queue->old_entries, old, head))
                goto retry;
            added_any_old_entries = true;
        }

        /* deactivate this queue */
        assert(seg->active);
        seg->active = false;
        dprintf(("deactivated queue %p\n", queue));

    } TREE_LOOP_END;

    tree_free(STM_PSEGMENT->active_queues);
    STM_PSEGMENT->active_queues = NULL;

    queue_lock_release();

    if (added_any_old_entries)
        cond_broadcast(C_QUEUE_OLD_ENTRIES);
}

void stm_queue_put(object_t *qobj, stm_queue_t *queue, object_t *newitem)
{
    /* must be run in a transaction, but doesn't cause conflicts or
       delays or transaction breaks.  you need to push roots!
    */
    stm_queue_segment_t *seg = &queue->segs[STM_SEGMENT->segment_num - 1];
    queue_entry_t *entry = malloc(sizeof(queue_entry_t));
    assert(entry);
    entry->object = newitem;
    entry->next = seg->added_in_this_transaction;
    seg->added_in_this_transaction = entry;

    queue_activate(queue);

    /* add qobj to 'objects_pointing_to_nursery' if it has the
       WRITE_BARRIER flag */
    if (qobj->stm_flags & GCFLAG_WRITE_BARRIER) {
        qobj->stm_flags &= ~GCFLAG_WRITE_BARRIER;
        LIST_APPEND(STM_PSEGMENT->objects_pointing_to_nursery, qobj);
    }
}

object_t *stm_queue_get(object_t *qobj, stm_queue_t *queue, double timeout,
                        stm_thread_local_t *tl)
{
    /* if the queue is empty, this commits and waits outside a transaction.
       must not be called if the transaction is atomic!  never causes
       conflicts.  you need to push roots!
    */
    struct timespec t;
    bool t_ready = false;
    queue_entry_t *entry;
    object_t *result;
    stm_queue_segment_t *seg = &queue->segs[STM_SEGMENT->segment_num - 1];

    if (seg->added_in_this_transaction) {
        entry = seg->added_in_this_transaction;
        seg->added_in_this_transaction = entry->next;
        if (entry == seg->added_young_limit)
            seg->added_young_limit = entry->next;
        result = entry->object;
        assert(result != NULL);
        free(entry);
        return result;
    }

 retry:
    entry = queue->old_entries;
    if (entry != NULL) {
        if (!__sync_bool_compare_and_swap(&queue->old_entries,
                                          entry, entry->next))
            goto retry;

        /* successfully popped the old 'entry'.  It remains in the
           'old_objects_popped' list for now. */
        entry->next = seg->old_objects_popped;
        seg->old_objects_popped = entry;

        queue_activate(queue);
        assert(entry->object != NULL);
        return entry->object;
    }
    else {
        /* no pending entry, wait */
#if STM_TESTS
        assert(timeout == 0.0);   /* can't wait in the basic tests */
#endif
        if (timeout == 0.0) {
            if (!stm_is_inevitable(tl)) {
                stm_become_inevitable(tl, "stm_queue_get");
                goto retry;
            }
            else
                return NULL;
        }

        STM_PUSH_ROOT(*tl, qobj);
        _stm_commit_transaction();

        s_mutex_lock();
        while (queue->old_entries == NULL) {
            if (timeout < 0.0) {      /* no timeout */
                cond_wait(C_QUEUE_OLD_ENTRIES);
            }
            else {
                if (!t_ready) {
                    timespec_delay(&t, timeout);
                    t_ready = true;
                }
                if (!cond_wait_timespec(C_QUEUE_OLD_ENTRIES, &t)) {
                    timeout = 0.0;   /* timed out! */
                    break;
                }
            }
        }
        s_mutex_unlock();

        _stm_start_transaction(tl);
        STM_POP_ROOT(*tl, qobj);   /* 'queue' should stay alive until here */
        goto retry;
    }
}

static void queue_trace_list(queue_entry_t *entry, void trace(object_t **),
                             queue_entry_t *stop_at)
{
    while (entry != stop_at) {
        trace(&entry->object);
        entry = entry->next;
    }
}

void stm_queue_tracefn(stm_queue_t *queue, void trace(object_t **))
{
    if (trace == TRACE_FOR_MAJOR_COLLECTION) {
        long i;
        for (i = 0; i < STM_NB_SEGMENTS; i++) {
            stm_queue_segment_t *seg = &queue->segs[i];
            seg->added_young_limit = seg->added_in_this_transaction;
            queue_trace_list(seg->added_in_this_transaction, trace, NULL);
            queue_trace_list(seg->old_objects_popped, trace, NULL);
        }
        queue_trace_list(queue->old_entries, trace, NULL);
    }
    else {
        /* for minor collections: it is enough to trace the objects
           added in the current transaction.  All other objects are
           old (or, worse, belong to a parallel thread and must not
           be traced). */
        stm_queue_segment_t *seg = &queue->segs[STM_SEGMENT->segment_num - 1];
        queue_trace_list(seg->added_in_this_transaction, trace,
                         seg->added_young_limit);
        seg->added_young_limit = seg->added_in_this_transaction;
    }
}
