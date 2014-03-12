/* ------------------------------------------------------------ */
#ifdef STM_DEBUGPRINT
/* ------------------------------------------------------------ */


#include <stdarg.h>


#define dprintf(args)   threadcolor_printf args
static inline int dprintfcolor(void)
{
    return 31 + STM_SEGMENT->segment_num % 6;
}

static int threadcolor_printf(const char *format, ...)
     __attribute__((format (printf, 1, 2)));

#ifdef STM_TESTS
#  define dprintf_test(args)   dprintf(args)
#else
#  define dprintf_test(args)   do { } while(0)
#endif


/* ------------------------------------------------------------ */
#else
/* ------------------------------------------------------------ */


#define dprintf(args)        do { } while(0)
#define dprintf_test(args)   do { } while(0)
#define dprintfcolor()       0


/* ------------------------------------------------------------ */
#endif
/* ------------------------------------------------------------ */


static void stm_fatalerror(const char *format, ...)
     __attribute__((format (printf, 1, 2), noreturn));
