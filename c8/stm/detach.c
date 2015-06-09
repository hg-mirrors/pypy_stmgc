#ifndef _STM_CORE_H_
# error "must be compiled via stmgc.c"
#endif


#define DETACHED_NO_THREAD  ((stm_thread_local_t *)-1)


stm_thread_local_t *volatile _stm_detached_inevitable_from_thread;


static void setup_detach(void)
{
    _stm_detached_inevitable_from_thread = NULL;
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

void _stm_reattach_transaction(stm_thread_local_t *old, stm_thread_local_t *tl)
{
    if (old != NULL) {
        /* We took over the inevitable transaction originally detached
           from a different thread.  We have to fix the %gs register if
           it is incorrect.  Careful, 'old' might be DETACHED_NO_THREAD.
        */
        int mysegnum = tl->last_associated_segment_num;

        if (STM_SEGMENT->segment_num != mysegnum) {
            set_gs_register(get_segment_base(mysegnum));
            assert(STM_SEGMENT->segment_num == mysegnum);
        }
        assert(old == DETACHED_NO_THREAD || STM_SEGMENT->running_thread == old);
        STM_SEGMENT->running_thread = tl;

        stm_safe_point();
    }
    else {
        /* there was no detached inevitable transaction */
        _stm_start_transaction(tl);
    }
}

static void fully_detach_thread(void)
{
    /* If there is a detached inevitable transaction, then make sure
       that it is "fully" detached.  The point is to make sure that
       the fast path of stm_enter_transactional_zone() will fail, and
       we'll call _stm_reattach_transaction(), which will in turn call
       stm_safe_point().  So a "fully detached" transaction will enter
       a safe point as soon as it is reattached.

       XXX THINK about concurrent threads here!
    */
    assert(_has_mutex());

 restart:
    stm_thread_local_t *old = stm_detached_inevitable_from_thread;
    if (old == NULL || old == DETACHED_NO_THREAD)
        return;

    if (!__sync_bool_compare_and_swap(&stm_detached_inevitable_from_thread,
                                      old, DETACHED_NO_THREAD))
        goto restart;
}
