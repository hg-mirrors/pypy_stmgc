#ifndef _STM_CORE_H
#define _STM_CORE_H

#include <stdint.h>

struct object_s {
    /* Every objects starts with one such structure */
    uint16_t modified;
    uint8_t modif_next;
    uint8_t flags;
};

void stm_setup(void);
void stm_setup_process(void);

void stm_start_transaction(void);
_Bool stm_stop_transaction(void);
struct object_s *stm_allocate(size_t size);

void stm_read(struct object_s *object);
void stm_write(struct object_s *object);
_Bool _stm_was_read(struct object_s *object);
_Bool _stm_was_written(struct object_s *object);

struct local_data_s *_stm_save_local_state(void);
void _stm_restore_local_state(struct local_data_s *p);
void _stm_teardown(void);
void _stm_teardown_process(void);

#endif
