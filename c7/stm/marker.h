
static void marker_fetch(stm_thread_local_t *tl, uintptr_t marker[2]);
static void marker_fetch_inev(void);
static void marker_expand(uintptr_t marker[2], char *segment_base,
                          char *outmarker);
static void marker_default_for_abort(struct stm_priv_segment_info_s *pseg);

static void marker_contention(int kind, bool abort_other,
                              uint8_t other_segment_num, object_t *obj);
