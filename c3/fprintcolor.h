#include <stdarg.h>


#define fprintf threadcolor_fprintf


int threadcolor_fprintf(FILE *stream, const char *format, ...)
     __attribute__((unused, format (printf, 2, 3), weak));

int threadcolor_fprintf(FILE *stream, const char *format, ...)
{
    char buffer[2048];
    va_list ap;
    int result;
    static __thread revision_t color = 0;
    if (color == 0) {
        static revision_t nextid = 0;
        while (1) {
            color = nextid;
            if (bool_cas(&nextid, color, color + 1))
                break;
        }
        color = 31 + color % 7;
    }
    int size = (int)sprintf(buffer, "\033[%dm", (int)color);
    assert(size >= 0);

    va_start(ap, format);
    result = vsnprintf(buffer + size, 2000, format, ap);
    assert(result >= 0);
    va_end(ap);

    strcpy(buffer + size + result, "\033[0m");
    fputs(buffer, stream);

    return result;
}
