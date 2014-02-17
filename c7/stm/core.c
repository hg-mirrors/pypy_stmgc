#ifndef _STM_CORE_H_
# error "must be compiled via stmgc.c"
#endif

#include <unistd.h>


static uint8_t write_locks[READMARKER_END - READMARKER_START];

static void teardown_core(void)
{
    memset(write_locks, 0, sizeof(write_locks));
}


void _stm_write_slowpath(object_t *obj)
{
    assert(_running_transaction());

    LIST_APPEND(STM_PSEGMENT->old_objects_to_trace, obj);

    /* for old objects from the same transaction, we are done now */
    if (obj_from_same_transaction(obj)) {
        obj->stm_flags |= GCFLAG_WRITE_BARRIER_CALLED;
        return;
    }


    /* otherwise, we need to privatize the pages containing the object,
       if they are still SHARED_PAGE.  The common case is that there is
       only one page in total. */
    size_t obj_size = 0;
    uintptr_t first_page = ((uintptr_t)obj) / 4096UL;
    uintptr_t page_count = 1;

    /* If the object is in the uniform pages of small objects (outside the
       nursery), then it fits into one page.  Otherwise, we need to compute
       it based on its location and size. */
    if ((obj->stm_flags & GCFLAG_SMALL_UNIFORM) == 0) {

        /* get the size of the object */
        obj_size = stmcb_size_rounded_up(
            (struct object_s *)REAL_ADDRESS(STM_SEGMENT->segment_base, obj));

        /* that's the page *following* the last page with the object */
        uintptr_t end_page = (((uintptr_t)obj) + obj_size + 4095) / 4096UL;

        page_count = end_page - first_page;
    }
    pages_privatize(first_page, page_count);


    /* do a read-barrier *before* the safepoints that may be issued in
       contention_management() */
    stm_read(obj);

    /* claim the write-lock for this object */
 retry:;
    uintptr_t lock_idx = (((uintptr_t)obj) >> 4) - READMARKER_START;
    uint8_t lock_num = STM_PSEGMENT->write_lock_num;
    uint8_t prev_owner;
    prev_owner = __sync_val_compare_and_swap(&write_locks[lock_idx],
                                             0, lock_num);

    /* if there was no lock-holder, we are done; otherwise... */
    if (UNLIKELY(prev_owner != 0)) {
        /* otherwise, call the contention manager, and then possibly retry.
           By construction it should not be possible that the owner
           of the object is already us */
        mutex_lock();
        contention_management(prev_owner - 1, true);
        mutex_unlock();
        goto retry;
    }

    /* add the write-barrier-already-called flag ONLY if we succeeded in
       getting the write-lock */
    assert(!(obj->stm_flags & GCFLAG_WRITE_BARRIER_CALLED));
    obj->stm_flags |= GCFLAG_WRITE_BARRIER_CALLED;
    LIST_APPEND(STM_PSEGMENT->modified_objects, obj);
}

static void reset_transaction_read_version(void)
{
    /* force-reset all read markers to 0 */

    /* XXX measure the time taken by this madvise() and the following
       zeroing of pages done lazily by the kernel; compare it with using
       16-bit read_versions.
    */
    /* XXX try to use madvise() on smaller ranges of memory.  In my
       measures, we could gain a factor 2 --- not really more, even if
       the range of virtual addresses below is very large, as long as it
       is already mostly non-reserved pages.  (The following call keeps
       them non-reserved; apparently the kernel just skips them very
       quickly.)
    */
    char *readmarkers = REAL_ADDRESS(STM_SEGMENT->segment_base,
                                     FIRST_READMARKER_PAGE * 4096UL);
    if (madvise(readmarkers, NB_READMARKER_PAGES * 4096UL,
                MADV_DONTNEED) < 0) {
        perror("madvise");
        abort();
    }
    reset_transaction_read_version_prebuilt();
    STM_SEGMENT->transaction_read_version = 1;
}

void _stm_start_transaction(stm_thread_local_t *tl, stm_jmpbuf_t *jmpbuf)
{
    mutex_lock();

    /* GS invalid before this point! */
    acquire_thread_segment(tl);

    assert(STM_PSEGMENT->safe_point == SP_NO_TRANSACTION);
    assert(STM_PSEGMENT->transaction_state == TS_NONE);
    STM_PSEGMENT->safe_point = SP_RUNNING;
    STM_PSEGMENT->transaction_state = (jmpbuf != NULL ? TS_REGULAR
                                                      : TS_INEVITABLE);
    STM_SEGMENT->jmpbuf_ptr = jmpbuf;

    mutex_unlock();

    uint8_t old_rv = STM_SEGMENT->transaction_read_version;
    STM_SEGMENT->transaction_read_version = old_rv + 1;
    if (UNLIKELY(old_rv == 0xff))
        reset_transaction_read_version();

    assert(list_is_empty(STM_PSEGMENT->modified_objects));
    assert(list_is_empty(STM_PSEGMENT->creation_markers));

    align_nursery_at_transaction_start();
}


/************************************************************/

#if NB_SEGMENTS != 2
# error "The logic in the functions below only works with two segments"
#endif

static bool detect_write_read_conflicts(void)
{
    long remote_num = 1 - STM_SEGMENT->segment_num;
    char *remote_base = get_segment_base(remote_num);
    uint8_t remote_version = get_segment(remote_num)->transaction_read_version;

    switch (get_priv_segment(remote_num)->transaction_state) {
    case TS_NONE:
    case TS_MUST_ABORT:
        return false;    /* no need to do any check */
    }

    LIST_FOREACH_R(
        STM_PSEGMENT->modified_objects,
        object_t * /*item*/,
        ({
            if (was_read_remote(remote_base, item, remote_version)) {
                /* A write-read conflict! */
                contention_management(remote_num, false);

                /* If we reach this point, it means we aborted the other
                   thread.  We're done here. */
                return true;
            }
        }));

    return false;
}

static void push_modified_to_other_segments(void)
{
    long remote_num = 1 - STM_SEGMENT->segment_num;
    char *local_base = STM_SEGMENT->segment_base;
    char *remote_base = get_segment_base(remote_num);
    bool remote_active =
        (get_priv_segment(remote_num)->transaction_state == TS_REGULAR ||
         get_priv_segment(remote_num)->transaction_state == TS_INEVITABLE);

    LIST_FOREACH_R(
        STM_PSEGMENT->modified_objects,
        object_t * /*item*/,
        ({
            if (remote_active) {
                assert(!was_read_remote(remote_base, item,
                          get_segment(remote_num)->transaction_read_version));
            }

            /* clear the write-lock (note that this runs with all other
               threads paused, so no need to be careful about ordering) */
            uintptr_t lock_idx = (((uintptr_t)item) >> 4) - READMARKER_START;
            assert(write_locks[lock_idx] == STM_PSEGMENT->write_lock_num);
            write_locks[lock_idx] = 0;

            /* remove again the WRITE_BARRIER_CALLED flag */
            assert(item->stm_flags & GCFLAG_WRITE_BARRIER_CALLED);
            item->stm_flags &= ~GCFLAG_WRITE_BARRIER_CALLED;

            /* copy the modified object to the other segment */
            char *src = REAL_ADDRESS(local_base, item);
            char *dst = REAL_ADDRESS(remote_base, item);
            ssize_t size = stmcb_size_rounded_up((struct object_s *)src);
            memcpy(dst, src, size);
        }));

    list_clear(STM_PSEGMENT->modified_objects);
}

void stm_commit_transaction(void)
{
    mutex_lock();

    assert(STM_PSEGMENT->safe_point = SP_RUNNING);
    STM_PSEGMENT->safe_point = SP_SAFE_POINT_CAN_COLLECT;

 restart:
    switch (STM_PSEGMENT->transaction_state) {

    case TS_REGULAR:
    case TS_INEVITABLE:
        break;

    case TS_MUST_ABORT:
        abort_with_mutex();

    default:
        assert(!"commit: bad transaction_state");
    }

    /* wait until the other thread is at a safe-point */
    if (!try_wait_for_other_safe_points(SP_SAFE_POINT_CANNOT_COLLECT))
        goto restart;

    /* the rest of this function runs either atomically without releasing
       the mutex, or it needs to restart. */

    /* detect conflicts */
    if (UNLIKELY(detect_write_read_conflicts()))
        goto restart;

    /* cannot abort any more from here */
    assert(STM_PSEGMENT->transaction_state != TS_MUST_ABORT);
    STM_SEGMENT->jmpbuf_ptr = NULL;

    /* copy modified object versions to other threads */
    push_modified_to_other_segments();

    /* done */
    stm_thread_local_t *tl = STM_SEGMENT->running_thread;
    release_thread_segment(tl);
    STM_PSEGMENT->safe_point = SP_NO_TRANSACTION;
    STM_PSEGMENT->transaction_state = TS_NONE;
    reset_all_creation_markers();

    mutex_unlock();
}

void stm_abort_transaction(void)
{
    mutex_lock();
    abort_with_mutex();
}

static void reset_modified_from_other_segments(void)
{
    /* pull the right versions from other threads in order
       to reset our pages as part of an abort */
    long remote_num = 1 - STM_SEGMENT->segment_num;
    char *local_base = STM_SEGMENT->segment_base;
    char *remote_base = get_segment_base(remote_num);

    LIST_FOREACH_R(
        STM_PSEGMENT->modified_objects,
        object_t * /*item*/,
        ({
            /* all objects in 'modified_objects' have this flag */
            assert(item->stm_flags & GCFLAG_WRITE_BARRIER_CALLED);

            /* memcpy in the opposite direction than
               push_modified_to_other_segments() */
            char *src = REAL_ADDRESS(remote_base, item);
            char *dst = REAL_ADDRESS(local_base, item);
            ssize_t size = stmcb_size_rounded_up((struct object_s *)src);
            memcpy(dst, src, size);

            /* copying from the other segment removed again the
               WRITE_BARRIER_CALLED flag */
            assert(!(item->stm_flags & GCFLAG_WRITE_BARRIER_CALLED));

            /* write all changes to the object before we release the
               write lock below.  This is needed because we need to
               ensure that if the write lock is not set, another thread
               can get it and then change 'src' in parallel.  The
               write_fence() ensures in particular that 'src' has been
               fully read before we release the lock: reading it
               is necessary to write 'dst'. */
            write_fence();

            /* clear the write-lock */
            uintptr_t lock_idx = (((uintptr_t)item) >> 4) - READMARKER_START;
            assert(write_locks[lock_idx]);
            write_locks[lock_idx] = 0;
        }));

    list_clear(STM_PSEGMENT->modified_objects);
}

static void abort_with_mutex(void)
{
    switch (STM_PSEGMENT->transaction_state) {
    case TS_REGULAR:
    case TS_MUST_ABORT:
        break;
    case TS_INEVITABLE:
        assert(!"abort: transaction_state == TS_INEVITABLE");
    default:
        assert(!"abort: bad transaction_state");
    }

    /* reset all the modified objects (incl. re-adding GCFLAG_WRITE_BARRIER) */
    reset_modified_from_other_segments();

    stm_jmpbuf_t *jmpbuf_ptr = STM_SEGMENT->jmpbuf_ptr;
    stm_thread_local_t *tl = STM_SEGMENT->running_thread;
    release_thread_segment(tl);
    STM_PSEGMENT->safe_point = SP_NO_TRANSACTION;
    STM_PSEGMENT->transaction_state = TS_NONE;
    reset_all_creation_markers();

    cond_broadcast();
    mutex_unlock();

    assert(jmpbuf_ptr != NULL);
    assert(jmpbuf_ptr != (stm_jmpbuf_t *)-1);    /* for tests only */
    __builtin_longjmp(*jmpbuf_ptr, 1);
}
