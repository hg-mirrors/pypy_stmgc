#ifndef _STM_CORE_H_
# error "must be compiled via stmgc.c"
#endif


/* _stm_detached_inevitable_from_thread is:

   - 0: there is no inevitable transaction, or it is not detached

   - a stm_thread_local_t pointer: this thread-local has detached its
     own inevitable transaction, and might concurrently reattach to it
     at any time

   - DETACHED_AND_FETCHED: another thread ran
     synchronize_all_threads(), so in order to reattach, the detaching
     thread must first go through s_mutex_lock()/s_mutex_unlock().
*/
#define DETACHED_AND_FETCHED  1

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

static struct stm_priv_segment_info_s *detached_and_fetched(void)
{
    long i;
    struct stm_priv_segment_info_s *result = NULL;
    for (i = 1; i < NB_SEGMENTS; i++) {
        if (get_priv_segment(i)->safe_point == SP_RUNNING_DETACHED_FETCHED) {
            assert(result == NULL);
            result = get_priv_segment(i);
        }
    }
    assert(result != NULL);
    return result;
}

void _stm_reattach_transaction(uintptr_t old, stm_thread_local_t *tl)
{
    if (old == 0) {
        /* there was no detached inevitable transaction */
        _stm_start_transaction(tl);
        return;
    }

    if (old == DETACHED_AND_FETCHED) {
        /* The detached transaction was fetched; wait until the s_mutex_lock
           is free.  The fetched transaction can only be reattached by the
           code here; there should be no risk of its state changing while
           we wait.
         */
        struct stm_priv_segment_info_s *pseg;
        s_mutex_lock();
        pseg = detached_and_fetched();
        pseg->safe_point = SP_RUNNING;
        old = (uintptr_t)pseg->running_thread;
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
    if (old < NB_SEGMENTS) {
        /* we have the mutex here, so this fetched detached transaction
           cannot get reattached in parallel */
        assert(get_priv_segment(old)->safe_point ==
               SP_RUNNING_DETACHED_FETCHED);
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

static void commit_own_inevitable_detached_transaction(stm_thread_local_t *tl)
{
    uintptr_t cur = _stm_detached_inevitable_from_thread;
    if ((cur & ~1) == (uintptr_t)tl) {
        stm_enter_transactional_zone(tl);
        _stm_commit_transaction();
    }
}

REWRITE:. we need a function to grab and commit the detached inev transaction
anyway.  So kill the special values of _stm_detached_inevitable_from_thread.
And call that function from core.c when we wait for the inev transaction to
finish
