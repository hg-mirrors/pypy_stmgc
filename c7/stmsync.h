
#include <stdint.h>

void stm_start_shared_lock(void);
void stm_stop_shared_lock(void);
void stm_stop_exclusive_lock(void);
void stm_start_exclusive_lock(void);
void _stm_start_safe_point(uint8_t flags);
void _stm_stop_safe_point(uint8_t flags);
void _stm_reset_shared_lock(void);
void _stm_grab_thread_segment(void);
void _stm_yield_thread_segment(void);

enum {
    LOCK_COLLECT = (1 << 0),
    LOCK_EXCLUSIVE = (1 << 1),
    THREAD_YIELD = (1 << 2),
};


void stm_request_safe_point(int thread_num);


#define NURSERY_CURRENT(tls)                                            \
            ((localchar_t *)(uintptr_t)(                                \
                (tls)->nursery_current_halfwords[1-LENDIAN]))

#define SET_NURSERY_CURRENT(tls, new_value)                             \
            ((tls)->nursery_current_halfwords[1-LENDIAN] =              \
                (uintptr_t)(new_value))
