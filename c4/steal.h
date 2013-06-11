#ifndef _SRCSTM_STEAL_H
#define _SRCSTM_STEAL_H


#define STUB_BLOCK_SIZE   (16 * WORD)    /* power of two */

#define STUB_THREAD(h)    (*(struct tx_public_descriptor **)           \
                            (((revision_t)(h)) & ~(STUB_BLOCK_SIZE-1)))

gcptr stm_stub_malloc(struct tx_public_descriptor *);
void stm_steal_stub(gcptr);
gcptr stm_get_stolen_obj(long index);   /* debugging */
void stm_normalize_stolen_objects(struct tx_descriptor *);
gcptr _stm_find_stolen_objects(struct tx_descriptor *, gcptr);


#endif
