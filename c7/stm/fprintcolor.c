/* ------------------------------------------------------------ */
#ifdef STM_DEBUGPRINT
/* ------------------------------------------------------------ */


int dprintfcolor(void)
{
    return 31 + STM_SEGMENT->segment_num % 6;
}

int threadcolor_printf(const char *format, ...)
{
    char buffer[2048];
    va_list ap;
    int result;
    int size = (int)sprintf(buffer, "\033[%dm", dprintfcolor());
    assert(size >= 0);

    va_start(ap, format);
    result = vsnprintf(buffer + size, 2000, format, ap);
    assert(result >= 0);
    va_end(ap);

    strcpy(buffer + size + result, "\033[0m");
    fputs(buffer, stderr);

    return result;
}


/* ------------------------------------------------------------ */
#endif
/* ------------------------------------------------------------ */
