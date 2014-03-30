#ifndef _STM_CORE_H_
# error "must be compiled via stmgc.c"
#endif


static inline void add_timing(stm_thread_local_t *tl, enum stm_time_e category,
                              double elapsed)
{
    tl->timing[category] += elapsed;
}

#define TIMING_CHANGE(tl, newstate)                     \
    double curtime = get_stm_time();                    \
    double elasped = curtime - tl->_timing_cur_start;   \
    enum stm_time_e oldstate = tl->_timing_cur_state;   \
    add_timing(tl, oldstate, elasped);                  \
    tl->_timing_cur_state = newstate;                   \
    tl->_timing_cur_start = curtime

static enum stm_time_e change_timing_state(enum stm_time_e newstate)
{
    stm_thread_local_t *tl = STM_SEGMENT->running_thread;
    TIMING_CHANGE(tl, newstate);
    return oldstate;
}

static void change_timing_state_tl(stm_thread_local_t *tl,
                                   enum stm_time_e newstate)
{
    TIMING_CHANGE(tl, newstate);
}

static void timing_end_transaction(enum stm_time_e attribute_to)
{
    stm_thread_local_t *tl = STM_SEGMENT->running_thread;
    TIMING_CHANGE(tl, STM_TIME_OUTSIDE_TRANSACTION);
    add_timing(tl, attribute_to, tl->timing[STM_TIME_RUN_CURRENT]);
    tl->timing[STM_TIME_RUN_CURRENT] = 0.0f;
}

void stm_flush_timing(stm_thread_local_t *tl)
{
    TIMING_CHANGE(tl, tl->_timing_cur_state);
}
