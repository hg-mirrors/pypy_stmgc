#ifndef _STM_CORE_H_
# error "must be compiled via stmgc.c"
#endif


/* _stm_detached_inevitable_from_thread is:

   - NULL: there is no inevitable transaction, or it is not detached

   - a stm_thread_local_t pointer: this thread-local has detached its
     own inevitable transaction, and might concurrently reattach to it
     at any time

   - a stm_thread_local_t pointer with the last bit set to 1: another
     thread ran synchronize_all_threads(), so in order to reattach,
     the detaching thread must first go through
     s_mutex_lock()/s_mutex_unlock().
*/
volatile uintptr_t _stm_detached_inevitable_from_thread;


static void setup_detach(void)
{
    _stm_detached_inevitable_from_thread = 0;
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

void _stm_reattach_transaction(uintptr_t old, stm_thread_local_t *tl)
{
    if (old == 0) {
        /* there was no detached inevitable transaction */
        _stm_start_transaction(tl);
        return;
    }

    if (old & 1) {
        /* The detached transaction was fetched; wait until the s_mutex_lock
           is free. 
         */
        stm_thread_local_t *old_tl;
        struct stm_priv_segment_info_s *pseg;

        old_tl = (stm_thread_local_t *)(--old);
        pseg = get_priv_segment(old_tl->last_associated_segment_num);
        assert(pseg->safe_point = SP_RUNNING_DETACHED_FETCHED);

        s_mutex_lock();
        pseg->safe_point = SP_RUNNING;
        s_mutex_unlock();
    }

    /* We took over the inevitable transaction originally detached
       from a different thread.  We have to fix the %gs register if
       it is incorrect.
    */
    ensure_gs_register(tl->last_associated_segment_num);
    assert(STM_SEGMENT->running_thread == (stm_thread_local_t *)old);
    STM_SEGMENT->running_thread = tl;

    stm_safe_point();
}

static bool fetch_detached_transaction(void)
{
    /* returns True if there is a detached transaction; afterwards, it
       is not necessary to call fetch_detached_transaction() again
       regularly.
    */
    uintptr_t old;
    stm_thread_local_t *tl;
    struct stm_priv_segment_info_s *pseg;
    assert(_has_mutex_here);

 restart:
    old = _stm_detached_inevitable_from_thread;
    if (old == 0)
        return false;
    if (old & 1) {
        /* we have the mutex here, so this detached transaction with the
           last bit set cannot reattach in parallel */
        tl = (stm_thread_local_t *)(old - 1);
        pseg = get_priv_segment(tl->last_associated_segment_num);
        assert(pseg->safe_point == SP_RUNNING_DETACHED_FETCHED);
        (void)pseg;
        return true;
    }

    if (!__sync_bool_compare_and_swap(&_stm_detached_inevitable_from_thread,
                                      old, old + 1))
        goto restart;

    tl = (stm_thread_local_t *)old;
    pseg = get_priv_segment(tl->last_associated_segment_num);
    assert(pseg->safe_point == SP_RUNNING);
    pseg->safe_point = SP_RUNNING_DETACHED_FETCHED;
    return true;
}

void stm_force_transaction_break(stm_thread_local_t *tl)
{
    assert(STM_SEGMENT->running_thread == tl);
    _stm_commit_transaction();
    _stm_start_transaction(tl);
}
