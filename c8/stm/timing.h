#include <time.h>

#define define_payload_types()                                              \
    stm_timing_event_payload_data_t stm_duration_data;                      \
    stm_timing_event_payload_t stm_duration_payload;

#define continue_timer() clock_gettime(CLOCK_MONOTONIC_RAW, &start);

/* Use raw monotonic time, i.e., solely based on local hardware (no NTP
   adjustments) as in prof.c to obtain values comparable with total program
   runtime. */
#define start_timer() struct timespec start, stop;                             \
                      struct timespec duration = { .tv_sec = 0, .tv_nsec = 0 };\
                      uint32_t nanosec_diff, sec_diff;                         \
                      define_payload_types()                                   \
                      continue_timer()

/* Must use start_timer before using this macro. */
#define get_duration() nanosec_diff = stop.tv_nsec - start.tv_nsec;         \
                       sec_diff = stop.tv_sec - start.tv_sec;               \
                       if (stop.tv_nsec < start.tv_nsec) {                  \
                           nanosec_diff += 1000000000;                      \
                           sec_diff -= 1;                                   \
                       }                                                    \
                       duration.tv_sec += sec_diff;                         \
                       duration.tv_nsec += nanosec_diff;                    \
                       if (duration.tv_nsec >= 1000000000) {                \
                           duration.tv_sec += 1;                            \
                           duration.tv_nsec -= 1000000000;                  \
                       }

#define pause_timer() clock_gettime(CLOCK_MONOTONIC_RAW, &stop);            \
                      get_duration()

#define reset_timer() duration.tv_sec = 0; duration.tv_nsec = 0;

#define stm_duration_payload(duration_data)                                 \
    stm_duration_data.duration = &(duration_data);                          \
    stm_duration_payload.type = STM_EVENT_PAYLOAD_DURATION;                 \
    stm_duration_payload.data = stm_duration_data;

#define publish_event(thread_local, event)                                  \
    (timing_enabled() ?                                                     \
        stmcb_timing_event((thread_local), (event), &stm_duration_payload) :\
        (void)0);

#define stop_timer_and_publish_for_thread(thread_local, event)              \
    pause_timer()                                                           \
    stm_duration_payload(duration)                                          \
    assert((thread_local) != NULL);                                         \
    publish_event((thread_local), (event))                                  \
    reset_timer()

#define stop_timer_and_publish(event)                                       \
    stop_timer_and_publish_for_thread(STM_SEGMENT->running_thread, (event))

#define set_payload(double_value)                                           \
    struct timespec payload_value = {                                       \
        .tv_sec = (int)(double_value),                                      \
        .tv_nsec = (int)(fmod((double_value), 1) * 1000000000),             \
    };

#define publish_custom_value_event(double_value, event)                     \
    set_payload((double_value))                                             \
    define_payload_types()                                                  \
    stm_duration_payload(payload_value);                                    \
    publish_event(STM_SEGMENT->running_thread, (event))
