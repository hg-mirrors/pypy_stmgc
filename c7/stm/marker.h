
static void marker_fetch(stm_thread_local_t *tl, uintptr_t marker[2]);
static void marker_expand(uintptr_t marker[2], char *segment_base,
                          char *outmarker);
static void marker_fetch_expand(struct stm_priv_segment_info_s *pseg);
static void marker_copy(stm_thread_local_t *tl,
                        struct stm_priv_segment_info_s *pseg,
                        enum stm_time_e attribute_to, double time);
static void marker_fetch_obj_write(uint8_t in_segment_num, object_t *obj,
                                   uintptr_t marker[2]);
static void marker_lookup_other_thread_write_write(uint8_t other_segment_num,
                                                   object_t *obj);
static void marker_lookup_other_thread_inev(uint8_t other_segment_num);
static void marker_lookup_same_thread_write_read(object_t *obj);
static void marker_fetch_inev(void);
