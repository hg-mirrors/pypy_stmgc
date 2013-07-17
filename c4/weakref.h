#ifndef _SRCSTM_WEAKREF_H
#define _SRCSTM_WEAKREF_H


void stm_move_young_weakrefs(struct tx_descriptor *);
void stm_invalidate_old_weakrefs(struct tx_public_descriptor *);


#endif
