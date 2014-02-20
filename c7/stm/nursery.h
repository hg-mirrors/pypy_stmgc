
/* special values of 'v_nursery_section_end' */
#define NSE_SIGNAL        1
#define NSE_SIGNAL_DONE   2

#if _STM_NSE_SIGNAL != NSE_SIGNAL
# error "adapt _STM_NSE_SIGNAL"
#endif

/* Rules for 'v_nursery_section_end':

   - Its main purpose is to be read by the owning thread in stm_allocate().

   - The owning thread can change its value without acquiring the mutex,
     but it must do so carefully, with a compare_and_swap.

   - If a different thread has the mutex, it can force the field to the
     value NSE_SIGNAL or NSE_SIGNAL_DONE with a regular write.  This should
     not be hidden by the compare_and_swap done by the owning thread:
     even if it occurs just before or just after a compare_and_swap,
     the end result is that the special value NSE_SIGNAL(_DONE) is still
     in the field.

   - When the owning thread sees NSE_SIGNAL, it must signal and wait until
     the other thread restores the value to NSE_SIGNAL_DONE.  When the
     owning thread sees NSE_SIGNAL_DONE, it can replace it, again with
     compare_and_swap, with the real value.

   - This should in theory be a volatile field, because it can be read
     from stm_allocate() while at the same time being changed to the value
     NSE_SIGNAL by another thread.  In practice, making it volatile has
     probably just a small negative impact on performance for no good reason.
*/

static void align_nursery_at_transaction_start(void);
static void restore_nursery_section_end(uintptr_t prev_value);

static inline bool was_read_remote(char *base, object_t *obj,
                                   uint8_t other_transaction_read_version,
                                   uint8_t min_read_version_outside_nursery);
