#ifndef _SRCSTM_DBGMEM_H
#define _SRCSTM_DBGMEM_H


#ifdef _GC_DEBUG

void *stm_malloc(size_t);
void stm_free(void *, size_t);

#else

#define stm_malloc(sz)    malloc(sz)
#define stm_free(p,sz)    free(p)

#endif


#endif
