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

static inline void timing_event_wt_inevitable(stm_thread_local_t *tl,
                          struct stm_priv_segment_info_s *other_pseg)
{
    /* We are not running a transaction yet; can't get the 'self loc' */
    assert(_has_mutex());
    if (stmcb_timing_event != NULL) {

        char outmarker[_STM_MARKER_LEN];
        acquire_marker_lock(other_pseg->pub.segment_base);
        marker_expand(other_pseg->marker_inev, other_pseg->pub.segment_base,
                      outmarker);
        release_marker_lock(other_pseg->pub.segment_base);

        stmcb_timing_event(tl, STM_WT_INEVITABLE, NULL, outmarker);
    }
}
