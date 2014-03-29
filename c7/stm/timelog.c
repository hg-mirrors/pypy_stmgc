#ifndef _STM_CORE_H_
# error "must be compiled via stmgc.c"
#endif


stm_timelog_t *stm_fetch_and_remove_timelog(stm_thread_local_t *tl)
{
    stm_timelog_t *tlog = tl->last_tlog;
    tl->last_tlog = NULL;
    return tlog;
}

void stm_free_timelog(stm_timelog_t *tlog)
{
    OPT_ASSERT(tlog != NULL);
    free(tlog);
}
