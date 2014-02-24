
/* '_stm_nursery_section_end' is either NURSERY_END or NSE_SIGNAL */
#define NSE_SIGNAL     _STM_NSE_SIGNAL


static uint32_t highest_overflow_number;

static void minor_collection(bool commit);
static void check_nursery_at_transaction_start(void);
