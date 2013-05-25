#ifndef _STMGC_H
#define _STMGC_H

#include <stdlib.h>
#include <stdint.h>


typedef intptr_t revision_t;

typedef struct stm_object_s {
    revision_t h_tid;
    revision_t h_revision;
} *gcptr;


/* by convention the lower half of _tid is used to store the object type */
#define stm_get_tid(o)       ((o)->h_tid & STM_USER_TID_MASK)
#define stm_set_tid(o, tid)  ((o)->h_tid = ((o)->h_tid & ~STM_USER_TID_MASK) \
                                           | (tid))

#define STM_SIZE_OF_USER_TID       (sizeof(revision_t) / 2)    /* in bytes */
#define STM_FIRST_GCFLAG           (1L << (8 * STM_SIZE_OF_USER_TID))
#define STM_USER_TID_MASK          (STM_FIRST_GCFLAG - 1)
#define PREBUILT_FLAGS             (STM_FIRST_GCFLAG * (2 + 8 + 64))
#define PREBUILT_REVISION          1


/* allocate an object out of the local nursery */
gcptr stm_allocate_object_of_size(size_t size);
gcptr stm_allocate(size_t size, unsigned long tid);

/* to push/pop objects into the local shadowstack */
/* (could be turned into macros or something later) */
void stm_push_root(gcptr);
gcptr stm_pop_root(void);

/* initialize/deinitialize the stm framework in the current thread */
void stm_initialize(void);
void stm_finalize(void);

/* read/write barriers (the most general versions only for now) */
gcptr stm_read_barrier(gcptr);
gcptr stm_write_barrier(gcptr);

/* start a new transaction, calls callback(), and when it returns
   finish that transaction.  callback() is called with the 'arg'
   provided, and with a retry_counter number.  Must save roots around
   this call. */
void stm_perform_transaction(gcptr arg, int (*callback)(gcptr, int));

/* finish the current transaction, start a new one.  Must save roots
   around calls to these two functions. */
void stm_commit_transaction(void);
void stm_begin_inevitable_transaction(void);

/* debugging: check if we're currently running a transaction or not. */
int stm_in_transaction(void);

/* change the default transaction length */
void stm_set_transaction_length(long length_max);

/* callback: get the size of an object */
extern size_t stmcb_size(gcptr);

/* callback: trace the content of an object */
extern void stmcb_trace(gcptr, void visit(gcptr *));

/* debugging: allocate but immediately old, not via the nursery */
gcptr _stm_allocate_object_of_size_old(size_t size);
gcptr _stm_allocate_old(size_t size, unsigned long tid);

/* You can put one GC-tracked thread-local object here.
   (Obviously it can be a container type containing more GC objects.)
   It is set to NULL by stm_initialize(). */
extern __thread gcptr stm_thread_local_obj;

#endif
