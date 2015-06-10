#ifndef _STM_CORE_H_
# error "must be compiled via stmgc.c"
#endif


/* _stm_detached_inevitable_segnum is:

   - -1: there is no inevitable transaction, or it is not detached

   - in range(1, NB_SEGMENTS): an inevitable transaction belongs to
     the segment and was detached.  It might concurrently be
     reattached at any time, with an XCHG (__sync_lock_test_and_set).
*/
volatile int _stm_detached_inevitable_seg_num;


static void setup_detach(void)
{
    _stm_detached_inevitable_seg_num = -1;
}


void _stm_leave_noninevitable_transactional_zone(void)
{
    _stm_become_inevitable(MSG_INEV_DONT_SLEEP);

    /* did it work? */
    if (STM_PSEGMENT->transaction_state == TS_INEVITABLE) {   /* yes */
        _stm_detach_inevitable_transaction(STM_SEGMENT->running_thread);
    }
    else {   /* no */
        _stm_commit_transaction();
    }
}

void _stm_reattach_transaction(int old, stm_thread_local_t *tl)
{
    if (old == -1) {
        /* there was no detached inevitable transaction */
        _stm_start_transaction(tl);
    }
    else {
        /* We took over the inevitable transaction originally detached
           from a different segment.  We have to fix the %gs register if
           it is incorrect.
        */
        tl->last_associated_segment_num = old;
        ensure_gs_register(old);
        assert(STM_PSEGMENT->transaction_state == TS_INEVITABLE);
        STM_SEGMENT->running_thread = tl;

        stm_safe_point();
    }
}

void stm_force_transaction_break(stm_thread_local_t *tl)
{
    assert(STM_SEGMENT->running_thread == tl);
    _stm_commit_transaction();
    _stm_start_transaction(tl);
}

static int fetch_detached_transaction(void)
{
    int cur = _stm_detached_inevitable_seg_num;
    if (cur != -1)
        cur = __sync_lock_test_and_set(    /* XCHG */
            &_stm_detached_inevitable_seg_num, -1);
    return cur;
}

static void commit_fetched_detached_transaction(int segnum)
{
    /* Here, 'seg_num' is the segment that contains the detached
       inevitable transaction from fetch_detached_transaction(),
       probably belonging to an unrelated thread.  We fetched it,
       which means that nobody else can concurrently fetch it now, but
       everybody will see that there is still a concurrent inevitable
       transaction.  This should guarantee there are not race
       conditions.
    */
    assert(segnum > 0);

    int mysegnum = STM_SEGMENT->segment_num;
    ensure_gs_register(segnum);

    assert(STM_PSEGMENT->transaction_state == TS_INEVITABLE);
    _stm_commit_transaction();   /* can't abort */

    ensure_gs_register(mysegnum);
}
