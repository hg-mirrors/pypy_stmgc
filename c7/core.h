#ifndef _STM_CORE_H
#define _STM_CORE_H

#include <stdint.h>
#include <stdbool.h>


#define TLPREFIX __attribute__((address_space(256)))

typedef TLPREFIX struct _thread_local1_s _thread_local1_t;
typedef TLPREFIX struct object_s object_t;
typedef TLPREFIX struct read_marker_s read_marker_t;


/* Structure of objects
   --------------------

   Objects manipulated by the user program, and managed by this library,
   must start with a "struct object_s" field.  Pointers to any user object
   must use the "TLPREFIX struct foo *" type --- don't forget TLPREFIX.
   The best is to use typedefs like above.

   The object_s part contains some fields reserved for the STM library,
   as well as a 32-bit integer field that can be freely used by the user
   program.  However, right now this field must be read-only --- i.e. it
   must never be modified on any object that may already belong to a
   past transaction; you can only set it on just-allocated objects.  The
   best is to consider it as a field that is written to only once on
   newly allocated objects.
*/

enum {
    GCFLAG_WRITE_BARRIER = (1 << 0),
};

struct object_s {
    uint8_t stm_flags;            /* reserved for the STM library */
    uint8_t stm_write_lock;       /* 1 if writeable by some thread */
    uint32_t header;              /* for the user program -- only write in
                                     newly allocated objects */
};

struct read_marker_s {
    uint8_t rm;
};

typedef intptr_t jmpbufptr_t[5];  /* for use with __builtin_setjmp() */

struct _thread_local1_s {
    jmpbufptr_t *jmpbufptr;
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
    if (UNLIKELY(obj->stm_flags & GCFLAG_WRITE_BARRIER))
        _stm_write_slowpath(obj);
}


/* must be provided by the user of this library */
extern size_t stm_object_size_rounded_up(object_t *);


#endif
