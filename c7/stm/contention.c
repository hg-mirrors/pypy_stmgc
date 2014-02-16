#ifndef _STM_CORE_H_
# error "must be compiled via stmgc.c"
#endif


static void contention_management(uint8_t other_segment_num, bool wait)
{
    /* A simple contention manager.  Called when some other thread
       holds the write lock on an object.  The current thread tries
       to do either a write or a read on it. */

    assert_has_mutex();
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
    else if (wait) {
        /* otherwise, we will issue a safe point and wait: */
        STM_PSEGMENT->safe_point = SP_SAFE_POINT_CANNOT_COLLECT;

        /* signal the other thread; it must abort */
        cond_broadcast();

        /* then wait, hopefully until the other thread broadcasts "I'm
           done aborting" (spurious wake-ups are ok) */
        cond_wait();

        /* now we return into _stm_write_slowpath() and will try again
           to acquire the write lock on our object. */
        STM_PSEGMENT->safe_point = SP_RUNNING;
    }
}
