#ifndef _STM_CORE_H_
# error "must be compiled via stmgc.c"
#endif


void _stm_write_slowpath(object_t *obj)
{
    abort();
}

void stm_start_transaction(stm_thread_local_t *tl, stm_jmpbuf_t *jmpbuf)
{
    /* GS invalid before this point! */
    _stm_stop_safe_point(LOCK_COLLECT|THREAD_YIELD);
    
}
