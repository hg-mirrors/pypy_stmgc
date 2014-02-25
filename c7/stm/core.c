#ifndef _STM_CORE_H_
# error "must be compiled via stmgc.c"
#endif


static void teardown_core(void)
{
    memset(write_locks, 0, sizeof(write_locks));
}


void _stm_write_slowpath(object_t *obj)
{
    assert(_running_transaction());
    assert(!_is_in_nursery(obj));

    /* is this an object from the same transaction, outside the nursery? */
    if ((obj->stm_flags & -GCFLAG_OVERFLOW_NUMBER_bit0) ==
            STM_PSEGMENT->overflow_number) {

        dprintf_test(("write_slowpath %p -> ovf obj_to_nurs\n", obj));
        obj->stm_flags &= ~GCFLAG_WRITE_BARRIER;
        assert(STM_PSEGMENT->objects_pointing_to_nursery != NULL);
        LIST_APPEND(STM_PSEGMENT->objects_pointing_to_nursery, obj);
        return;
    }

    /* do a read-barrier now.  Note that this must occur before the
       safepoints that may be issued in contention_management(). */
    stm_read(obj);

    /* claim the write-lock for this object.  In case we're running the
       same transaction since a long while, the object can be already in
       'modified_old_objects' (but, because it had GCFLAG_WRITE_BARRIER,
       not in 'objects_pointing_to_nursery').  We'll detect this case
       by finding that we already own the write-lock. */
    uintptr_t lock_idx = (((uintptr_t)obj) >> 4) - WRITELOCK_START;
    uint8_t lock_num = STM_PSEGMENT->write_lock_num;
    assert((intptr_t)lock_idx >= 0);
 retry:
    if (write_locks[lock_idx] == 0) {
        if (UNLIKELY(!__sync_bool_compare_and_swap(&write_locks[lock_idx],
                                                   0, lock_num)))
            goto retry;

        dprintf_test(("write_slowpath %p -> mod_old\n", obj));

        /* First change to this old object from this transaction.
           Add it to the list 'modified_old_objects'. */
        LIST_APPEND(STM_PSEGMENT->modified_old_objects, obj);

        /* We need to privatize the pages containing the object, if they
           are still SHARED_PAGE.  The common case is that there is only
           one page in total. */
        uintptr_t first_page = ((uintptr_t)obj) / 4096UL;

        /* If the object is in the uniform pages of small objects
           (outside the nursery), then it fits into one page.  This is
           the common case. Otherwise, we need to compute it based on
           its location and size. */
        if ((obj->stm_flags & GCFLAG_SMALL_UNIFORM) != 0) {
            pages_privatize(first_page, 1, true);
        }
        else {
            char *realobj;
            size_t obj_size;
            uintptr_t end_page;

            /* get the size of the object */
            realobj = REAL_ADDRESS(STM_SEGMENT->segment_base, obj);
            obj_size = stmcb_size_rounded_up((struct object_s *)realobj);

            /* that's the page *following* the last page with the object */
            end_page = (((uintptr_t)obj) + obj_size + 4095) / 4096UL;

            pages_privatize(first_page, end_page - first_page, true);
        }
    }
    else if (write_locks[lock_idx] == lock_num) {
        OPT_ASSERT(STM_PSEGMENT->objects_pointing_to_nursery != NULL);
#ifdef STM_TESTS
        bool found = false;
        LIST_FOREACH_R(STM_PSEGMENT->modified_old_objects, object_t *,
                       ({ if (item == obj) { found = true; break; } }));
        assert(found);
#endif
    }
    else {
        /* call the contention manager, and then retry (unless we were
           aborted). */
        write_write_contention_management(lock_idx);
        goto retry;
    }

    /* A common case for write_locks[] that was either 0 or lock_num:
       we need to add the object to 'objects_pointing_to_nursery'
       if there is such a list. */
    if (STM_PSEGMENT->objects_pointing_to_nursery != NULL) {
        dprintf_test(("write_slowpath %p -> old obj_to_nurs\n", obj));
        LIST_APPEND(STM_PSEGMENT->objects_pointing_to_nursery, obj);
    }

    /* add the write-barrier-already-called flag ONLY if we succeeded in
       getting the write-lock */
    assert(obj->stm_flags & GCFLAG_WRITE_BARRIER);
    obj->stm_flags &= ~GCFLAG_WRITE_BARRIER;

    /* for sanity, check that all other segment copies of this object
       still have the flag */
    long i;
    for (i = 0; i < NB_SEGMENTS; i++) {
        assert(i == STM_SEGMENT->segment_num ||
               (((struct object_s *)REAL_ADDRESS(get_segment_base(i), obj))
                ->stm_flags & GCFLAG_WRITE_BARRIER));
    }
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
    dprintf(("reset_transaction_read_version: %p %ld\n", readmarkers,
             (long)(NB_READMARKER_PAGES * 4096UL)));
    if (mmap(readmarkers, NB_READMARKER_PAGES * 4096UL,
             PROT_READ | PROT_WRITE,
             MAP_FIXED | MAP_PAGES_FLAGS, -1, 0) != readmarkers) {
        /* fall-back */
#if STM_TESTS
        stm_fatalerror("reset_transaction_read_version: %m\n");
#endif
        memset(readmarkers, 0, NB_READMARKER_PAGES * 4096UL);
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
    STM_PSEGMENT->shadowstack_at_start_of_transaction = tl->shadowstack;
    STM_SEGMENT->nursery_end = NURSERY_END;

    dprintf(("start_transaction\n"));

    mutex_unlock();

    uint8_t old_rv = STM_SEGMENT->transaction_read_version;
    STM_SEGMENT->transaction_read_version = old_rv + 1;
    if (UNLIKELY(old_rv >= 0xfe)) {
        /* reset if transaction_read_version was 0xfe or 0xff.  If it's
           0xff, then we need it because the new value would overflow to
           0.  But resetting it already from 0xfe is better for short
           or medium transactions: at the next minor collection we'll
           still have one free number to increase to. */
        reset_transaction_read_version();
    }

    assert(list_is_empty(STM_PSEGMENT->modified_old_objects));
    assert(STM_PSEGMENT->objects_pointing_to_nursery == NULL);
    assert(STM_PSEGMENT->large_overflow_objects == NULL);

    check_nursery_at_transaction_start();
}


/************************************************************/

#if NB_SEGMENTS != 2
# error "The logic in the functions below only works with two segments"
#endif

static void detect_write_read_conflicts(void)
{
    long remote_num = 1 - STM_SEGMENT->segment_num;
    char *remote_base = get_segment_base(remote_num);
    uint8_t remote_version = get_segment(remote_num)->transaction_read_version;

    switch (get_priv_segment(remote_num)->transaction_state) {
    case TS_NONE:
    case TS_MUST_ABORT:
        return;    /* no need to do any check */
    default:;
    }

    LIST_FOREACH_R(
        STM_PSEGMENT->modified_old_objects,
        object_t * /*item*/,
        ({
            if (was_read_remote(remote_base, item, remote_version)) {
                /* A write-read conflict! */
                contention_management(remote_num);

                /* If we reach this point, it means we aborted the other
                   thread.  We're done here. */
                assert(get_priv_segment(remote_num)->transaction_state ==
                       TS_MUST_ABORT);
                return;
            }
        }));
}

static void synchronize_overflow_object_now(object_t *obj)
{
    assert(!_is_in_nursery(obj));
    assert((obj->stm_flags & GCFLAG_SMALL_UNIFORM) == 0);

    char *realobj = REAL_ADDRESS(STM_SEGMENT->segment_base, obj);
    ssize_t obj_size = stmcb_size_rounded_up((struct object_s *)realobj);
    uintptr_t start = (uintptr_t)obj;
    uintptr_t end = start + obj_size;
    uintptr_t first_page = start / 4096UL;
    uintptr_t last_page = (end - 1) / 4096UL;

    do {
        if (flag_page_private[first_page] != SHARED_PAGE) {
            /* The page is a PRIVATE_PAGE.  We need to diffuse this fragment
               of our object from our own segment to all other segments. */

            uintptr_t copy_size;
            if (first_page == last_page) {
                /* this is the final fragment */
                copy_size = end - start;
            }
            else {
                /* this is a non-final fragment, going up to the page's end */
                copy_size = 4096 - (start & 4095);
            }

            /* double-check that the result fits in one page */
            assert(copy_size > 0);
            assert(copy_size + (start & 4095) <= 4096);

            long i;
            char *src = REAL_ADDRESS(STM_SEGMENT->segment_base, start);
            for (i = 0; i < NB_SEGMENTS; i++) {
                if (i != STM_SEGMENT->segment_num) {
                    char *dst = REAL_ADDRESS(get_segment_base(i), start);
                    memcpy(dst, src, copy_size);
                }
            }
        }

        start = (start + 4096) & ~4095;
    } while (first_page++ < last_page);
}

static void push_overflow_objects_from_privatized_pages(void)
{
    if (STM_PSEGMENT->large_overflow_objects == NULL)
        return;

    LIST_FOREACH_R(STM_PSEGMENT->large_overflow_objects, object_t *,
                   synchronize_overflow_object_now(item));
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
        STM_PSEGMENT->modified_old_objects,
        object_t * /*item*/,
        ({
            if (remote_active) {
                assert(!was_read_remote(remote_base, item,
                    get_segment(remote_num)->transaction_read_version));
            }

            /* clear the write-lock (note that this runs with all other
               threads paused, so no need to be careful about ordering) */
            uintptr_t lock_idx = (((uintptr_t)item) >> 4) - WRITELOCK_START;
            assert((intptr_t)lock_idx >= 0);
            assert(write_locks[lock_idx] == STM_PSEGMENT->write_lock_num);
            write_locks[lock_idx] = 0;

            /* the WRITE_BARRIER flag should have been set again by
               minor_collection() */
            assert((item->stm_flags & GCFLAG_WRITE_BARRIER) != 0);

            /* copy the modified object to the other segment */
            char *src = REAL_ADDRESS(local_base, item);
            char *dst = REAL_ADDRESS(remote_base, item);
            ssize_t size = stmcb_size_rounded_up((struct object_s *)src);
            memcpy(dst, src, size);
        }));

    list_clear(STM_PSEGMENT->modified_old_objects);
}

static void _finish_transaction(void)
{
    /* signal all the threads blocked in wait_for_other_safe_points() */
    if (STM_SEGMENT->nursery_end == NSE_SIGNAL) {
        STM_SEGMENT->nursery_end = NURSERY_END;
        cond_broadcast(C_SAFE_POINT);
    }

    STM_PSEGMENT->safe_point = SP_NO_TRANSACTION;
    STM_PSEGMENT->transaction_state = TS_NONE;

    /* reset these lists to NULL for the next transaction */
    LIST_FREE(STM_PSEGMENT->objects_pointing_to_nursery);
    LIST_FREE(STM_PSEGMENT->large_overflow_objects);

    stm_thread_local_t *tl = STM_SEGMENT->running_thread;
    release_thread_segment(tl);
    /* cannot access STM_SEGMENT or STM_PSEGMENT from here ! */
}

void stm_commit_transaction(void)
{
    assert(!_has_mutex());
    assert(STM_PSEGMENT->safe_point == SP_RUNNING);

    bool has_any_overflow_object =
        (STM_PSEGMENT->objects_pointing_to_nursery != NULL);

    minor_collection(/*commit=*/ true);

    mutex_lock();
    STM_PSEGMENT->safe_point = SP_SAFE_POINT_CAN_COLLECT;

    /* wait until the other thread is at a safe-point */
    wait_for_other_safe_points(SP_SAFE_POINT_CANNOT_COLLECT);

    /* the rest of this function either runs atomically without
       releasing the mutex, or aborts the current thread. */

    /* detect conflicts */
    detect_write_read_conflicts();

    /* cannot abort any more from here */
    dprintf(("commit_transaction\n"));

    assert(STM_PSEGMENT->transaction_state != TS_MUST_ABORT);
    STM_SEGMENT->jmpbuf_ptr = NULL;

    /* synchronize overflow objects living in privatized pages */
    push_overflow_objects_from_privatized_pages();

    /* synchronize modified old objects to other threads */
    push_modified_to_other_segments();

    /* update 'overflow_number' if needed */
    if (has_any_overflow_object) {
        highest_overflow_number += GCFLAG_OVERFLOW_NUMBER_bit0;
        assert(highest_overflow_number != 0);   /* XXX else, overflow! */
        STM_PSEGMENT->overflow_number = highest_overflow_number;
    }

    /* done */
    _finish_transaction();

    /* wake up one other thread waiting for a segment. */
    cond_signal(C_RELEASE_THREAD_SEGMENT);

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
        STM_PSEGMENT->modified_old_objects,
        object_t * /*item*/,
        ({
            /* memcpy in the opposite direction than
               push_modified_to_other_segments() */
            char *src = REAL_ADDRESS(remote_base, item);
            char *dst = REAL_ADDRESS(local_base, item);
            ssize_t size = stmcb_size_rounded_up((struct object_s *)src);
            memcpy(dst, src, size);

            /* objects in 'modified_old_objects' usually have the
               WRITE_BARRIER flag, unless they have been modified
               recently.  Ignore the old flag; after copying from the
               other segment, we should have the flag. */
            assert(item->stm_flags & GCFLAG_WRITE_BARRIER);

            /* write all changes to the object before we release the
               write lock below.  This is needed because we need to
               ensure that if the write lock is not set, another thread
               can get it and then change 'src' in parallel.  The
               write_fence() ensures in particular that 'src' has been
               fully read before we release the lock: reading it
               is necessary to write 'dst'. */
            write_fence();

            /* clear the write-lock */
            uintptr_t lock_idx = (((uintptr_t)item) >> 4) - WRITELOCK_START;
            assert((intptr_t)lock_idx >= 0);
            assert(write_locks[lock_idx] == STM_PSEGMENT->write_lock_num);
            write_locks[lock_idx] = 0;
        }));

    list_clear(STM_PSEGMENT->modified_old_objects);
}

static void abort_with_mutex(void)
{
    dprintf(("~~~ ABORT\n"));

    switch (STM_PSEGMENT->transaction_state) {
    case TS_REGULAR:
    case TS_MUST_ABORT:
        break;
    case TS_INEVITABLE:
        assert(!"abort: transaction_state == TS_INEVITABLE");
    default:
        assert(!"abort: bad transaction_state");
    }

    /* throw away the content of the nursery */
    throw_away_nursery();

    /* reset all the modified objects (incl. re-adding GCFLAG_WRITE_BARRIER) */
    reset_modified_from_other_segments();

    stm_jmpbuf_t *jmpbuf_ptr = STM_SEGMENT->jmpbuf_ptr;
    stm_thread_local_t *tl = STM_SEGMENT->running_thread;
    tl->shadowstack = STM_PSEGMENT->shadowstack_at_start_of_transaction;

    _finish_transaction();

    /* wake up one other thread waiting for a segment.  In order to support
       contention.c, we use a broadcast, to make sure that all threads are
       signalled, including the one that requested an abort, if any.
       Moreover, we wake up any thread waiting for this one to do a safe
       point, if any.
    */
    cond_broadcast(C_RELEASE_THREAD_SEGMENT);

    mutex_unlock();

    assert(jmpbuf_ptr != NULL);
    assert(jmpbuf_ptr != (stm_jmpbuf_t *)-1);    /* for tests only */
    __builtin_longjmp(*jmpbuf_ptr, 1);
}

void _stm_become_inevitable(char *msg)
{
    long i;

    mutex_lock();
    switch (STM_PSEGMENT->transaction_state) {

    case TS_INEVITABLE:
        break;   /* already good */

    case TS_REGULAR:
        /* become inevitable */
        for (i = 0; i < NB_SEGMENTS; i++) {
            if (get_priv_segment(i)->transaction_state == TS_INEVITABLE) {
                abort_with_mutex();
            }
        }
        STM_PSEGMENT->transaction_state = TS_INEVITABLE;
        break;

    case TS_MUST_ABORT:
        abort_with_mutex();

    default:
        assert(!"invalid transaction_state in become_inevitable");
    }
    mutex_unlock();
}
