#include <stdarg.h>


#define fprintf threadcolor_fprintf


int threadcolor_fprintf(FILE *stream, const char *format, ...)
     __attribute__((format (printf, 2, 3)));
