#include <stdarg.h>
#include "stmimpl.h"


void stm_fatalerror(const char *format, ...)
     __attribute__((format (printf, 1, 2), noreturn));


#ifdef _GC_DEBUG

#define dprintf(args)   threadcolor_printf args
int dprintfcolor(void);

int threadcolor_printf(const char *format, ...)
     __attribute__((format (printf, 1, 2)));

#else

#define dprintf(args)   do { } while(0)
#define dprintfcolor()  0

#endif
