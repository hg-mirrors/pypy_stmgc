
uint32_t stm_queue_entry_userdata;


typedef union stm_queue_segment_u {
    struct {
        /* a chained list of fresh entries that have been allocated and
           added to this queue during the current transaction.  If the
           transaction commits, these are moved to 'old_entries'. */
        stm_queue_entry_t *added_in_this_transaction;

        /* a chained list of old entries that the current transaction
           popped.  only used if the transaction is not inevitable:
           if it aborts, these entries are added back to 'old_entries'. */
        stm_queue_entry_t *old_objects_popped;

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
    stm_queue_entry_t *volatile old_entries;
};


stm_queue_t *stm_queue_create(void)
{
    void *mem;
    int result = posix_memalign(&mem, 64, sizeof(stm_queue_t));
    assert(result == 0);
    memset(mem, 0, sizeof(stm_queue_t));
    return (stm_queue_t *)mem;
}

void stm_queue_free(stm_queue_t *queue)
{
    long i;
    for (i = 0; i < STM_NB_SEGMENTS; i++) {
        /* it is possible that queues_deactivate_all() runs in parallel,
           but it should not be possible at this point for another thread
           to change 'active' from false to true.  if it is false, then
           that's it */
        if (!queue->segs[i].active)
            continue;

        struct stm_priv_segment_info_s *pseg = get_priv_segment(i);
        spinlock_acquire(pseg->active_queues_lock);

        if (queue->segs[i].active) {
            assert(pseg->active_queues != NULL);
            bool ok = tree_delete_item(pseg->active_queues, (uintptr_t)queue);
            assert(ok);
        }

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
        stm_queue_entry_t *head;

        if (at_commit)
            head = seg->added_in_this_transaction;
        else
            head = seg->old_objects_popped;

        /* move the list of entries that must survive to 'old_entries' */
        if (head != NULL) {
            stm_queue_entry_t *old;
            stm_queue_entry_t* tail = head;
            while (tail->next != NULL)
                tail = tail->next;
         retry:
            old = queue->old_entries;
            tail->next = old;
            if (!__sync_bool_compare_and_swap(&queue->old_entries, old, head))
                goto retry;
            added_any_old_entries = true;
        }

        /* forget the two lists of entries */
        seg->added_in_this_transaction = NULL;
        seg->old_objects_popped = NULL;

        /* deactivate this queue */
        assert(seg->active);
        seg->active = false;

    } TREE_LOOP_END;

    tree_free(STM_PSEGMENT->active_queues);
    STM_PSEGMENT->active_queues = NULL;

    queue_lock_release();

    if (added_any_old_entries)
        cond_broadcast(C_QUEUE_OLD_ENTRIES);
}

void stm_queue_put(stm_queue_t *queue, object_t *newitem)
{
    /* must be run in a transaction, but doesn't cause conflicts or
       delays or transaction breaks.  you need to push roots!
    */
    stm_queue_segment_t *seg = &queue->segs[STM_SEGMENT->segment_num - 1];
    stm_queue_entry_t *entry = (stm_queue_entry_t *)
        stm_allocate(sizeof(stm_queue_entry_t));
    entry->userdata = stm_queue_entry_userdata;
    entry->object = newitem;
    entry->next = seg->added_in_this_transaction;
    seg->added_in_this_transaction = entry;

    queue_activate(queue);
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
    stm_queue_entry_t *entry;
    stm_queue_segment_t *seg = &queue->segs[STM_SEGMENT->segment_num - 1];

    if (seg->added_in_this_transaction) {
        entry = seg->added_in_this_transaction;
        seg->added_in_this_transaction = entry->next;
        return entry->object; /* 'entry' is left behind for the GC to collect */
    }

 retry:
    entry = queue->old_entries;
    if (entry != NULL) {
        if (!__sync_bool_compare_and_swap(&queue->old_entries,
                                          entry, entry->next))
            goto retry;

        /* successfully popped the old 'entry' */
        entry->next = seg->old_objects_popped;
        seg->old_objects_popped = entry;

        queue_activate(queue);
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

static void queue_trace_segment(stm_queue_segment_t *seg,
                                void trace(object_t **)) {
    trace((object_t **)&seg->added_in_this_transaction);
    trace((object_t **)&seg->old_objects_popped);
}

void stm_queue_tracefn(stm_queue_t *queue, void trace(object_t **))
{
    if (trace == TRACE_FOR_MAJOR_COLLECTION) {
        long i;
        for (i = 0; i < STM_NB_SEGMENTS; i++)
            queue_trace_segment(&queue->segs[i], trace);
        trace((object_t **)&queue->old_entries);
    }
    else {
        queue_trace_segment(&queue->segs[STM_SEGMENT->segment_num - 1],
                            trace);
    }
}
