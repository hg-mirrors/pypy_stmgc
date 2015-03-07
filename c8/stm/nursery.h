
#define NSE_SIGPAUSE   _STM_NSE_SIGNAL_MAX
#define NSE_SIGABORT   _STM_NSE_SIGNAL_ABORT

static uint32_t highest_overflow_number;

static void _cards_cleared_in_object(struct stm_priv_segment_info_s *pseg, object_t *obj,
                                     bool strict);
static void _reset_object_cards(struct stm_priv_segment_info_s *pseg,
                                object_t *obj, uint8_t mark_value,
                                bool mark_all, bool really_clear);

static void minor_collection(bool commit);
static void check_nursery_at_transaction_start(void);
static size_t throw_away_nursery(struct stm_priv_segment_info_s *pseg);
static void major_do_validation_and_minor_collections(void);

static void assert_memset_zero(void *s, size_t n);


static inline bool is_abort(uintptr_t nursery_end) {
    return (nursery_end <= _STM_NSE_SIGNAL_MAX && nursery_end != NSE_SIGPAUSE);
}

static inline bool is_aborting_now(uint8_t other_segment_num) {
    return (is_abort(get_segment(other_segment_num)->nursery_end) &&
            get_priv_segment(other_segment_num)->safe_point != SP_RUNNING);
}


#define must_abort()   is_abort(STM_SEGMENT->nursery_end)
static object_t *find_shadow(object_t *obj);
