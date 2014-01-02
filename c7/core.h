#ifndef _STM_CORE_H
#define _STM_CORE_H

#include <stdint.h>
#include <stdbool.h>
#include <setjmp.h>


#define TLPREFIX __attribute__((address_space(256)))

typedef TLPREFIX struct _thread_local1_s _thread_local1_t;
typedef TLPREFIX struct object_s object_t;
typedef TLPREFIX struct read_marker_s read_marker_t;


struct object_s {
    uint16_t write_version;
    /*uint8_t stm_flags;*/
};

struct read_marker_s {
    uint8_t rm;
};

struct _thread_local1_s {
    jmp_buf *jmpbufptr;
    uint8_t transaction_read_version;
    uint16_t transaction_write_version;
};
#define _STM_TL1            ((_thread_local1_t *)4352)


/* this should use llvm's coldcc calling convention,
   but it's not exposed to C code so far */
void _stm_write_slowpath(object_t *);

#define LIKELY(x)   __builtin_expect(x, true)
#define UNLIKELY(x) __builtin_expect(x, false)


static inline void stm_read(object_t *obj)
{
    ((read_marker_t *)(((uintptr_t)obj) >> 4))->rm =
        _STM_TL1->transaction_read_version;
}

static inline void stm_write(object_t *obj)
{
    if (UNLIKELY(obj->write_version != _STM_TL1->transaction_write_version))
        _stm_write_slowpath(obj);
}


/* must be provided by the user of this library */
extern size_t stm_object_size_rounded_up(object_t *);


#endif
