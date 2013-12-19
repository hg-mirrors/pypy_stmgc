#ifndef _STM_CORE_H
#define _STM_CORE_H

#include <stdint.h>

struct object_s {
    /* Every objects starts with one such structure */
    uint16_t modified;
    uint8_t modif_next;
    uint8_t flags;
};

struct _read_marker_s {
    /* We associate a single byte to every object, by simply dividing
       the address of the object by 16.  The number in this single byte
       gives the last time we have read the object.  See stm_read(). */
    unsigned char c;
};

extern struct _read_marker_s *stm_current_read_markers;
extern uint16_t stm_transaction_version;


/************************************************************/

void stm_setup(void);
void stm_setup_process(void);

void stm_start_transaction(void);
_Bool stm_stop_transaction(void);
struct object_s *stm_allocate(size_t size);

static inline void stm_read(struct object_s *object)
{
    stm_current_read_markers[((uintptr_t)object) >> 4].c =
        (unsigned char)(uintptr_t)stm_current_read_markers;
}

void _stm_write_slowpath(struct object_s *);

static inline void stm_write(struct object_s *object)
{
    if (__builtin_expect(object->modified != stm_transaction_version, 0))
        _stm_write_slowpath(object);
}

_Bool _stm_was_read(struct object_s *object);
_Bool _stm_was_written(struct object_s *object);

struct local_data_s *_stm_save_local_state(void);
void _stm_restore_local_state(struct local_data_s *p);
void _stm_teardown(void);
void _stm_teardown_process(void);

#endif
