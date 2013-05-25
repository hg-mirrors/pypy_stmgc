#ifndef _SRCSTM_DBGMEM_H
#define _SRCSTM_DBGMEM_H


#ifdef _GC_DEBUG

void *stm_malloc(size_t);
void stm_free(void *, size_t);

/* Debugging: for tracking which memory regions should be read or not. */
void stm_dbgmem_not_used(void *p, size_t size, int protect);
void stm_dbgmem_used_again(void *p, size_t size, int protect);
int stm_dbgmem_is_active(void *p, int allow_outside);

#else

#define stm_malloc(sz)                 malloc(sz)
#define stm_free(p,sz)                 free(p)
#define stm_dbgmem_not_used(p,sz,i)    /* nothing */
#define stm_dbgmem_used_again(p,sz,i)  /* nothing */
#define stm_dbgmem_is_active(p,i)      1

#endif


#endif
