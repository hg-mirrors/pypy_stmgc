#ifndef _STM_CORE_H
#define _STM_CORE_H

#include <stdint.h>

#define GCOBJECT __attribute__((address_space(256)))

#define GCFLAG_WRITE_BARRIER  0x01

typedef GCOBJECT struct object_s {
    /* Every objects starts with one such structure */
    uint8_t flags;
} object_t;

struct _read_marker_s {
    /* We associate a single byte to every object, by simply dividing
       the address of the object by 16.  The number in this single byte
       gives the last time we have read the object.  See stm_read(). */
    unsigned char c;
};

typedef GCOBJECT struct _thread_local1_s {
    uint8_t read_marker;
} _thread_local1_t;

#define _STM_TL1   (((_thread_local1_t *)0)[-1])

#define _STM_CRM   ((GCOBJECT struct _read_marker_s *)0)


/************************************************************/

void stm_setup(void);
int stm_setup_thread(void);

void stm_start_transaction(void);
_Bool stm_stop_transaction(void);
object_t *stm_allocate(size_t size);

static inline void stm_read(object_t *object)
{
    _STM_CRM[((uintptr_t)object) >> 4].c = _STM_TL1.read_marker;
}

void _stm_write_barrier_slowpath(object_t *);

static inline void stm_write(object_t *object)
{
    if (__builtin_expect((object->flags & GCFLAG_WRITE_BARRIER) != 0, 0))
        _stm_write_barrier_slowpath(object);
}

_Bool _stm_was_read(object_t *object);
_Bool _stm_was_written(object_t *object);

void _stm_restore_state_for_thread(int thread_num);
void _stm_teardown(void);
void _stm_teardown_thread(void);

#endif
