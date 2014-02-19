/* ------------------------------------------------------------ */
#ifdef STM_DEBUGPRINT
/* ------------------------------------------------------------ */


#include <stdarg.h>


#define dprintf(args)   threadcolor_printf args
static int dprintfcolor(void);

static int threadcolor_printf(const char *format, ...)
     __attribute__((format (printf, 1, 2)));


/* ------------------------------------------------------------ */
#else
/* ------------------------------------------------------------ */


#define dprintf(args)   do { } while(0)
#define dprintfcolor()  0


/* ------------------------------------------------------------ */
#endif
/* ------------------------------------------------------------ */


static void stm_fatalerror(const char *format, ...)
     __attribute__((format (printf, 1, 2), noreturn));
