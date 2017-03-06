#include <time.h>

/* Use raw monotonic time, i.e., solely based on local hardware (no NTP
   adjustments) as in prof.c to obtain values comparable with total program
   runtime. */
#define start_timer() struct timespec start;                                 \
                      clock_gettime(CLOCK_MONOTONIC_RAW, &start);

#define stop_timer() struct timespec stop;                                   \
                     clock_gettime(CLOCK_MONOTONIC_RAW, &stop);

/* Must use start_timer and stop_timer before using this macro. */
#define get_duration() struct timespec duration = {                          \
                           stop.tv_sec - start.tv_sec,                       \
                           stop.tv_nsec - start.tv_nsec                      \
                       };

#define stm_duration_payload(duration)                                       \
    stm_timing_event_payload_data_t stm_duration_data =                      \
        { .duration = &duration };                                           \
    stm_timing_event_payload_t stm_duration_payload =                        \
        { STM_EVENT_PAYLOAD_DURATION, stm_duration_data };

#define publish_event(event)                                                 \
    stmcb_timing_event(STM_SEGMENT->running_thread, event, &stm_duration_payload);

#define stop_timer_and_publish(event) stop_timer()                           \
                                      get_duration()                         \
                                      stm_duration_payload(duration)         \
                                      publish_event(event)
