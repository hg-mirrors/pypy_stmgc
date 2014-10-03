void (*stmcb_timing_event)(stm_thread_local_t *, enum stm_event_e,
                           const char *, const char *);

static inline void timing_event(stm_thread_local_t *tl,
                                enum stm_event_e event,
                                const char *marker1,
                                const char *marker2)
{
    if (stmcb_timing_event != NULL)
        stmcb_timing_event(tl, event, marker1, marker2);
}
