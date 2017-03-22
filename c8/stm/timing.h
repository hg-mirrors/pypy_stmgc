#include <time.h>

#define continue_timer() clock_gettime(CLOCK_MONOTONIC_RAW, &start);

/* Use raw monotonic time, i.e., solely based on local hardware (no NTP
   adjustments) as in prof.c to obtain values comparable with total program
   runtime. */
#define start_timer() struct timespec start, stop;                             \
                      struct timespec duration = { .tv_sec = 0, .tv_nsec = 0 };\
                      uint32_t nanosec_diff, sec_diff;                         \
                      continue_timer()

/* Must use start_timer before using this macro. */
#define get_duration() nanosec_diff = stop.tv_nsec - start.tv_nsec;         \
                       sec_diff = stop.tv_sec - start.tv_sec;               \
                       if (stop.tv_nsec < start.tv_nsec) {                  \
                           nanosec_diff += 1000000000;                      \
                           sec_diff -= 1;                                   \
                       }                                                    \
                       duration.tv_sec += sec_diff;                         \
                       duration.tv_nsec += nanosec_diff;

#define pause_timer() clock_gettime(CLOCK_MONOTONIC_RAW, &stop);            \
                      get_duration()

#define stm_duration_payload(duration)                                      \
    stm_timing_event_payload_data_t stm_duration_data =                     \
        { .duration = &duration };                                          \
    stm_timing_event_payload_t stm_duration_payload =                       \
        { STM_EVENT_PAYLOAD_DURATION, stm_duration_data };

#define publish_event(thread_local, event)                                  \
    (timing_enabled() ?                                                     \
        stmcb_timing_event(thread_local, event, &stm_duration_payload) :    \
        (void)0);

#define stop_timer_and_publish_for_thread(thread_local, event)              \
    pause_timer()                                                           \
    stm_duration_payload(duration)                                          \
    assert(thread_local != NULL);                                           \
    publish_event(thread_local, event)

#define stop_timer_and_publish(event)                                       \
    stop_timer_and_publish_for_thread(STM_SEGMENT->running_thread, event)
