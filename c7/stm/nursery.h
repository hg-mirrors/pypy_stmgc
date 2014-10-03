
/* 'nursery_end' is either NURSERY_END, NSE_SIGxxx, or STM_TR_ABORT_xxx. */
#define NSE_SIGPAUSE        (_STM_NSE_SIGNAL_MAX - 1)
#define NSE_SIGCOMMITSOON   (_STM_NSE_SIGNAL_MAX - 2)

#if !(STM_TR_ABORT_OTHER < NSE_SIGCOMMITSOON)
#  error "STM_TR_ABORT_xxx is too large; increase _STM_NSE_SIGNAL_MAX"
#endif


static uint32_t highest_overflow_number;

static void _cards_cleared_in_object(struct stm_priv_segment_info_s *pseg, object_t *obj);
static void _reset_object_cards(struct stm_priv_segment_info_s *pseg,
                                object_t *obj, uint8_t mark_value,
                                bool mark_all);
static void minor_collection(bool commit);
static void check_nursery_at_transaction_start(void);
static size_t throw_away_nursery(struct stm_priv_segment_info_s *pseg);
static void major_do_minor_collections(void);

#define must_abort()   is_abort(STM_SEGMENT->nursery_end)

static void assert_memset_zero(void *s, size_t n);

static object_t *find_shadow(object_t *obj);
