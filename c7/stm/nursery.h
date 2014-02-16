
/* special values of 'v_nursery_section_end' */
#define NSE_SIGNAL        1
#define NSE_SIGNAL_DONE   2

static void align_nursery_at_transaction_start(void);
static void restore_nursery_section_end(uintptr_t prev_value);
