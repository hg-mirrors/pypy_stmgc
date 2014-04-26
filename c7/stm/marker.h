
static void marker_fetch(stm_thread_local_t *tl, uintptr_t marker[2]);
static void marker_expand(uintptr_t marker[2], char *segment_base,
                          char *outmarker);
static void marker_fetch_expand(struct stm_priv_segment_info_s *pseg);
static void marker_copy(stm_thread_local_t *tl,
                        struct stm_priv_segment_info_s *pseg,
                        enum stm_time_e attribute_to, double time);
static void lookup_other_thread_recorded_marker(uint8_t other_segment_num,
                                                object_t *obj);
