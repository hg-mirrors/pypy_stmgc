#ifndef _SRCSTM_WEAKREF_H
#define _SRCSTM_WEAKREF_H


#define WEAKREF_PTR(wr, sz)  ((object_t * TLPREFIX *)(((stm_char *)(wr)) + (sz) - sizeof(void*)))

void stm_move_young_weakrefs(void);
void stm_visit_old_weakrefs(void);


#endif
