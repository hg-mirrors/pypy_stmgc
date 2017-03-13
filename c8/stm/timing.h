#include <time.h>

#define continue_timer() clock_gettime(CLOCK_MONOTONIC_RAW, &start);

/* Use raw monotonic time, i.e., solely based on local hardware (no NTP
   adjustments) as in prof.c to obtain values comparable with total program
   runtime. */
#define start_timer() struct timespec start, stop, duration = { 0, 0 };      \
                      continue_timer()

/* Must use start_timer before using this macro. */
#define get_duration() duration.tv_sec =                                     \
                          stop.tv_sec - start.tv_sec + duration.tv_sec;      \
                       duration.tv_nsec =                                    \
                          stop.tv_nsec - start.tv_nsec + duration.tv_nsec;

#define pause_timer() clock_gettime(CLOCK_MONOTONIC_RAW, &stop);             \
                      get_duration()

#define stm_duration_payload(duration)                                       \
    stm_timing_event_payload_data_t stm_duration_data =                      \
        { .duration = &duration };                                           \
    stm_timing_event_payload_t stm_duration_payload =                        \
        { STM_EVENT_PAYLOAD_DURATION, stm_duration_data };

#define publish_event(event)                                                 \
    (timing_enabled() ?                                                      \
        stmcb_timing_event(                                                  \
            STM_SEGMENT->running_thread, event, &stm_duration_payload) :     \
        (void)0);

#define stop_timer_and_publish(event) pause_timer()                          \
                                      stm_duration_payload(duration)         \
                                      publish_event(event)
