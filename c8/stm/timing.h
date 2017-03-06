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
