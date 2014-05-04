#ifndef _STM_CORE_H_
# error "must be compiled via stmgc.c"
#endif


static inline void add_timing(stm_thread_local_t *tl, enum stm_time_e category,
                              double elapsed)
{
    tl->timing[category] += elapsed;
    tl->events[category] += 1;
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

static double change_timing_state_tl(stm_thread_local_t *tl,
                                     enum stm_time_e newstate)
{
    TIMING_CHANGE(tl, newstate);
    return elasped;
}

static void timing_end_transaction(enum stm_time_e attribute_to)
{
    stm_thread_local_t *tl = STM_SEGMENT->running_thread;
    TIMING_CHANGE(tl, STM_TIME_OUTSIDE_TRANSACTION);
    double time_this_transaction = tl->timing[STM_TIME_RUN_CURRENT];
    add_timing(tl, attribute_to, time_this_transaction);
    tl->timing[STM_TIME_RUN_CURRENT] = 0.0f;

    if (attribute_to != STM_TIME_RUN_COMMITTED) {
        struct stm_priv_segment_info_s *pseg =
            get_priv_segment(STM_SEGMENT->segment_num);
        marker_copy(tl, pseg, attribute_to, time_this_transaction);
    }
}

static const char *timer_names[] = {
    "outside transaction",
    "run current",
    "run committed",
    "run aborted write write",
    "run aborted write read",
    "run aborted inevitable",
    "run aborted other",
    "wait free segment",
    "wait write read",
    "wait inevitable",
    "wait other",
    "sync commit soon",
    "bookkeeping",
    "minor gc",
    "major gc",
    "sync pause",
};

void stm_flush_timing(stm_thread_local_t *tl, int verbose)
{
    enum stm_time_e category = tl->_timing_cur_state;
    uint64_t oldevents = tl->events[category];
    TIMING_CHANGE(tl, category);
    tl->events[category] = oldevents;

    assert((sizeof(timer_names) / sizeof(timer_names[0])) == _STM_TIME_N);
    if (verbose > 0) {
        int i;
        s_mutex_lock();
        fprintf(stderr, "thread %p:\n", tl);
        for (i = 0; i < _STM_TIME_N; i++) {
            fprintf(stderr, "    %-24s %9u %8.3f s\n",
                    timer_names[i], tl->events[i], (double)tl->timing[i]);
        }
        fprintf(stderr, "    %-24s %6s %11.6f s\n",
                "longest recorded marker", "", tl->longest_marker_time);
        fprintf(stderr, "    \"%.*s\"\n",
                (int)_STM_MARKER_LEN, tl->longest_marker_self);
        s_mutex_unlock();
    }
}
