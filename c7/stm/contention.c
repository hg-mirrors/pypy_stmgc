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
        other_pseg->pub.nursery_end = NSE_SIGABORT;
    }
    else if (other_pseg->start_time <= STM_PSEGMENT->start_time) {
        /* The other thread started before us, so I should abort, as I'm
           the least long-running transaction. */
    }
    else if (other_pseg->transaction_state == TS_REGULAR) {
        /* The other thread started strictly after us.  We tell it
           to abort if we can (e.g. if it's not TS_INEVITABLE). */
        other_pseg->pub.nursery_end = NSE_SIGABORT;
    }

    /* Now check what we just did... almost: the check at the following
       line can also find a NSE_SIGABORT that was set earlier.
    */
    if (other_pseg->pub.nursery_end != NSE_SIGABORT) {
        /* if the other thread is not in aborting-soon mode, then *we*
           must abort. */
        abort_with_mutex();
    }
}

static void write_write_contention_management(uintptr_t lock_idx)
{
    s_mutex_lock();
    if (must_abort())
        abort_with_mutex();

    uint8_t prev_owner = ((volatile uint8_t *)write_locks)[lock_idx];
    if (prev_owner != 0 && prev_owner != STM_PSEGMENT->write_lock_num) {

        uint8_t other_segment_num = prev_owner - 1;
        contention_management(other_segment_num);

        /* The rest of this code is for the case where we continue to
           run.  We have to signal the other thread to abort, and wait
           until it does. */

        int sp = get_priv_segment(other_segment_num)->safe_point;
        switch (sp) {

        case SP_RUNNING:
            /* The other thread is running now, so if we set
               NSE_SIGABORT in 'nursery_end', it will soon enter a
               mutex_lock() and thus abort.  Note that this line can
               overwrite a NSE_SIGPAUSE, which is fine.
            */
            get_segment(other_segment_num)->nursery_end = NSE_SIGABORT;
            break;

        /* The other cases are where the other thread is at a
           safe-point.  We wake it up by sending the correct signal.
        */
        case SP_WAIT_FOR_C_REQUEST_REMOVED:
            cond_broadcast(C_REQUEST_REMOVED);
            break;

        case SP_WAIT_FOR_C_AT_SAFE_POINT:
            cond_broadcast(C_AT_SAFE_POINT);
            break;

#ifdef STM_TESTS
        case SP_WAIT_FOR_OTHER_THREAD:
            /* abort anyway for tests.  We can't wait here */
            abort_with_mutex();
#endif

        default:
            stm_fatalerror("unexpected other_pseg->safe_point: %d", sp);
        }

        /* wait, hopefully until the other thread broadcasts "I'm
           done aborting" (spurious wake-ups are ok).  Important:
           this is not a safe point of any kind!  The shadowstack
           is not correct here.  It should not end in a deadlock,
           because the target thread is, in principle, guaranteed
           to call abort_with_mutex().
        */
        dprintf(("contention: wait C_ABORTED...\n"));
        cond_wait(C_ABORTED);
        dprintf(("contention: done\n"));

        if (must_abort())
            abort_with_mutex();

        /* now we return into _stm_write_slowpath() and will try again
           to acquire the write lock on our object. */
    }

    s_mutex_unlock();
}
