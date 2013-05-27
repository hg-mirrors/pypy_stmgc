#include "stmimpl.h"

static __thread revision_t tcolor = 0;
static revision_t tnextid = 0;


int threadcolor_fprintf(FILE *stream, const char *format, ...)
{
    char buffer[2048];
    va_list ap;
    int result;
    if (tcolor == 0) {
        while (1) {
            tcolor = tnextid;
            if (bool_cas(&tnextid, tcolor, tcolor + 1))
                break;
        }
        tcolor = 31 + tcolor % 7;
    }
    int size = (int)sprintf(buffer, "\033[%dm", (int)tcolor);
    assert(size >= 0);

    va_start(ap, format);
    result = vsnprintf(buffer + size, 2000, format, ap);
    assert(result >= 0);
    va_end(ap);

    strcpy(buffer + size + result, "\033[0m");
    fputs(buffer, stream);

    return result;
}
