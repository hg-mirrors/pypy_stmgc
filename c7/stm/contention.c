#ifndef _STM_CORE_H_
# error "must be compiled via stmgc.c"
#endif


static void contention_management(uint8_t other_segment_num)
{
    /* A simple contention manager.  Called when some other thread
       holds the write lock on an object.  The current thread tries
       to do either a write or a read on it. */

    assert(_has_mutex());
    assert(other_segment_num != STM_SEGMENT->segment_num);

    /* Who should abort here: this thread, or the other thread? */
    struct stm_priv_segment_info_s* other_pseg;
    other_pseg = get_priv_segment(other_segment_num);

    if (STM_PSEGMENT->transaction_state == TS_INEVITABLE) {
        /* I'm inevitable, so the other is not. */
        assert(other_pseg->transaction_state != TS_INEVITABLE);
        other_pseg->transaction_state = TS_MUST_ABORT;
    }
    else if (other_pseg->start_time < STM_PSEGMENT->start_time) {
        /* The other thread started before us, so I should abort, as I'm
           the least long-running transaction. */
    }
    else if (other_pseg->transaction_state == TS_REGULAR) {
        /* The other thread started strictly after us.  We tell it
           to abort if we can (e.g. if it's not TS_INEVITABLE). */
        other_pseg->transaction_state = TS_MUST_ABORT;
    }

    if (other_pseg->transaction_state != TS_MUST_ABORT) {
        /* if the other thread is not in aborting-soon mode, then we must
           abort. */
        abort_with_mutex();
    }
    else {
        /* signal the other thread; it must abort.

           Note that we know that the target thread is running now, and
           so it is or will soon be blocked at a mutex_lock() or a
           cond_wait(C_SAFE_POINT).  Thus broadcasting C_SAFE_POINT is
           enough to wake it up in the second case.
        */
        cond_broadcast(C_SAFE_POINT);
    }
}

static void write_write_contention_management(uintptr_t lock_idx)
{
    mutex_lock();

    if (STM_PSEGMENT->transaction_state == TS_MUST_ABORT)
        abort_with_mutex();

    uint8_t prev_owner = ((volatile uint8_t *)write_locks)[lock_idx];
    if (prev_owner != 0 && prev_owner != STM_PSEGMENT->write_lock_num) {

        uint8_t other_segment_num = prev_owner - 1;
        contention_management(other_segment_num);

        /* the rest of this code is for the case where we continue to
           run, and the other thread is asked to abort */

#ifdef STM_TESTS
        /* abort anyway for tests. We mustn't call cond_wait() */
        abort_with_mutex();
#endif

        /* first mark the other thread as "needing a safe-point" */
        struct stm_priv_segment_info_s* other_pseg;
        other_pseg = get_priv_segment(other_segment_num);
        assert(other_pseg->transaction_state == TS_MUST_ABORT);
        other_pseg->pub.nursery_end = NSE_SIGNAL;

        /* we will issue a safe point and wait: */
        STM_PSEGMENT->safe_point = SP_SAFE_POINT_CANNOT_COLLECT;

        /* wait, hopefully until the other thread broadcasts "I'm
           done aborting" (spurious wake-ups are ok). */
        cond_wait(C_SAFE_POINT);

        cond_broadcast(C_RESUME);

        /* now we return into _stm_write_slowpath() and will try again
           to acquire the write lock on our object. */
        STM_PSEGMENT->safe_point = SP_RUNNING;
    }

    mutex_unlock();
}
