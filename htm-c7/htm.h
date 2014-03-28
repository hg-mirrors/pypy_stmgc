#ifndef _HTM_H
#define _HTM_H

#include <stdio.h>
#include <assert.h>
#include <pthread.h>



#define XBEGIN_OK              (~0)
#define XBEGIN_UNKNOWN         (0)
#define XBEGIN_XABORT          (1 << 0)
#define XBEGIN_MAYBE_RETRY     (1 << 1)
#define XBEGIN_NORMAL_CONFLICT (1 << 2)
#define XBEGIN_BUFFER_OVERFLOW (1 << 3)
#define XBEGIN_DEBUG           (1 << 4)
#define XBEGIN_NESTED_ABORT    (1 << 5)
#define XBEGIN_XABORT_ARG(x) (((x) >> 24) & 0xFF)

static __thread char buf[128];
__attribute__((unused))
static char* xbegin_status(int status)
{
    if (status == XBEGIN_OK)
        snprintf(buf, 128, "OK");
    else if (status == XBEGIN_UNKNOWN)
        snprintf(buf, 128, "UNKNOWN");
    else if (status & XBEGIN_XABORT)
        snprintf(buf, 128, "XABORT(%d)", XBEGIN_XABORT_ARG(status));
    else if (status & XBEGIN_MAYBE_RETRY)
        snprintf(buf, 128, "MAYBE_RETRY");
    else if (status & XBEGIN_NORMAL_CONFLICT)
        snprintf(buf, 128, "NORMAL_CONFLICT");
    else if (status & XBEGIN_BUFFER_OVERFLOW)
        snprintf(buf, 128, "BUFFER_OVERFLOW");
    else if (status & XBEGIN_DEBUG)
        snprintf(buf, 128, "DEBUG");
    else if (status & XBEGIN_NESTED_ABORT)
        snprintf(buf, 128, "NESTED_ABORT");
    else
        snprintf(buf, 128, "WAT.");

    return buf;
}

static __attribute__((__always_inline__)) inline int xbegin()
{
    int result = XBEGIN_OK;
    asm volatile(".byte 0xC7, 0xF8; .long 0" : "+a" (result) :: "memory");
    return result;
}

static __attribute__((__always_inline__)) inline void xend()
{
    asm volatile(".byte 0x0F, 0x01, 0xD5" ::: "memory");
}

#define xabort(argument) do { asm volatile(".byte 0xC6, 0xF8, %P0" :: "i" (argument) : "memory"); } while (0);

static __attribute__((__always_inline__)) inline int xtest()
{
    unsigned char result;
    asm volatile(".byte 0x0F, 0x01, 0xD6; setnz %0" : "=r" (result) :: "memory");
    return result;
}


static __attribute__((__always_inline__)) inline int mutex_locked(pthread_mutex_t* mut)
{
    /* HACK: pthread internals! */
    return !!mut->__data.__lock;
}



#endif
