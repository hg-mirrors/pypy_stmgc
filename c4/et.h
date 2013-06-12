/*** Extendable Timestamps
 *
 * Documentation:
 * doc-*.txt
 *
 * This is very indirectly based on rstm_r5/stm/et.hpp.
 * See http://www.cs.rochester.edu/research/synchronization/rstm/api.shtml
 */

#ifndef _SRCSTM_ET_H
#define _SRCSTM_ET_H


#define MAX_THREADS         1024
#define LOCKED              (INTPTR_MAX - 2*(MAX_THREADS-1))
#define WORD                sizeof(gcptr)

/* Description of the flags
 * ------------------------
 *
 * Objects are either "young" or "old" depending on the generational garbage
 * collection: "young" objects are the ones in the nursery (plus a few big
 * ones outside) and will be collected by the following minor collection.
 *
 * Additionally, objects are either "public", "protected" or "private".
 *
 * GCFLAG_OLD is set on old objects.
 *
 * GCFLAG_VISITED is used temporarily during major collections.
 *
 * GCFLAG_PUBLIC is set on public objects.
 *
 * GCFLAG_BACKUP_COPY means the object is a (protected) backup copy.
 * For debugging.
 *
 * GCFLAG_PUBLIC_TO_PRIVATE is added to a *public* object that has got a
 * *private* copy.  It is sticky, reset only at the next major collection.
 *
 * GCFLAG_PREBUILT_ORIGINAL is only set on the original version of
 * prebuilt objects.
 *
 * GCFLAG_WRITE_BARRIER is set on *old* objects to track old-to- young
 * pointers.  It may be left set on *public* objects but is ignored
 * there, because public objects are read-only.  The flag is removed
 * once a write occurs and the object is recorded in the list
 * 'old_pointing_to_young'; it is set again at the next minor
 * collection.
 *
 * GCFLAG_NURSERY_MOVED is used temporarily during minor collections.
 *
 * GCFLAG_STUB is set for debugging on stub objects made by stealing or
 * by major collections.  'p_stub->h_revision' might be a value
 * that is == 2 (mod 4): in this case they point to a protected/private
 * object that belongs to the thread 'STUB_THREAD(p_stub)'.
 */
#define GCFLAG_OLD               (STM_FIRST_GCFLAG << 0)
#define GCFLAG_VISITED           (STM_FIRST_GCFLAG << 1)
#define GCFLAG_PUBLIC            (STM_FIRST_GCFLAG << 2)
#define GCFLAG_PREBUILT_ORIGINAL (STM_FIRST_GCFLAG << 3)
#define GCFLAG_PUBLIC_TO_PRIVATE (STM_FIRST_GCFLAG << 4)
#define GCFLAG_WRITE_BARRIER     (STM_FIRST_GCFLAG << 5)
#define GCFLAG_NURSERY_MOVED     (STM_FIRST_GCFLAG << 6)
#define GCFLAG_BACKUP_COPY       (STM_FIRST_GCFLAG << 7)   /* debugging */
#define GCFLAG_STUB              (STM_FIRST_GCFLAG << 8)   /* debugging */
#define GCFLAG_PRIVATE_FROM_PROTECTED (STM_FIRST_GCFLAG << 9)

/* this value must be reflected in PREBUILT_FLAGS in stmgc.h */
#define GCFLAG_PREBUILT  (GCFLAG_VISITED           | \
                          GCFLAG_PREBUILT_ORIGINAL | \
                          GCFLAG_OLD               | \
                          GCFLAG_PUBLIC)

#define GC_FLAG_NAMES  { "OLD",               \
                         "VISITED",           \
                         "PUBLIC",            \
                         "PREBUILT_ORIGINAL", \
                         "PUBLIC_TO_PRIVATE", \
                         "WRITE_BARRIER",     \
                         "NURSERY_MOVED",     \
                         "BACKUP_COPY",       \
                         "STUB",              \
                         "PRIVATE_FROM_PROTECTED", \
                         NULL }

/************************************************************/

#define ABRT_MANUAL               0
#define ABRT_COMMIT               1
#define ABRT_STOLEN_MODIFIED      2
#define ABRT_VALIDATE_INFLIGHT    3
#define ABRT_VALIDATE_COMMIT      4
#define ABRT_VALIDATE_INEV        5
#define ABRT_COLLECT_MINOR        6
#define ABRT_COLLECT_MAJOR        7
#define ABORT_REASONS         8

#define SPLP_ABORT                0
#define SPLP_LOCKED_INFLIGHT      1
#define SPLP_LOCKED_VALIDATE      2
#define SPLP_LOCKED_COMMIT        3
#define SPINLOOP_REASONS      4

/* this struct contains thread-local data that may be occasionally
 * accessed by a foreign thread and that must stay around after the
 * thread shuts down.  It is reused the next time a thread starts. */
struct tx_public_descriptor {
  revision_t collection_lock;
  struct tx_descriptor *descriptor;
  struct stub_block_s *stub_blocks;
  gcptr stub_free_list;
  struct GcPtrList stolen_objects;
  revision_t free_list_next;
  /* xxx gcpage data here */
};

/* this struct contains all thread-local data that is never accessed
 * by a foreign thread */
struct tx_descriptor {
  struct tx_public_descriptor *public_descriptor;
  revision_t public_descriptor_index;
  jmp_buf *setjmp_buf;
  revision_t start_time;
  revision_t my_lock;
  gcptr *shadowstack;
  gcptr **shadowstack_end_ref;

  long atomic;   /* 0 = not atomic, > 0 atomic */
  unsigned long count_reads;
  unsigned long reads_size_limit;        /* see should_break_tr. */
  unsigned long reads_size_limit_nonatomic;
  int active;    /* 0 = inactive, 1 = regular, 2 = inevitable,
                    negative = killed by collection */
  struct timespec start_real_time;
  int max_aborts;
  unsigned int num_commits;
  unsigned int num_aborts[ABORT_REASONS];
  unsigned int num_spinloops[SPINLOOP_REASONS];
  struct GcPtrList list_of_read_objects;
  //struct GcPtrList abortinfo;
  struct GcPtrList private_from_protected;
  struct G2L public_to_private;
  char *longest_abort_info;
  long long longest_abort_info_time;
  revision_t *private_revision_ref;
  struct FXCache recent_reads_cache;
};

extern __thread struct tx_descriptor *thread_descriptor;
extern __thread revision_t stm_private_rev_num;
extern struct tx_public_descriptor *stm_descriptor_array[];

/************************************************************/


void BeginTransaction(jmp_buf *);
void BeginInevitableTransaction(void);  /* must save roots around this call */
void CommitTransaction(void);           /* must save roots around this call */
void BecomeInevitable(const char *why); /* must save roots around this call */
void AbortTransaction(int);
void AbortTransactionAfterCollect(struct tx_descriptor *, int);
void AbortNowIfDelayed(void);
void SpinLoop(int);

gcptr stm_DirectReadBarrier(gcptr);
gcptr stm_RepeatReadBarrier(gcptr);
gcptr stm_WriteBarrier(gcptr);
gcptr _stm_nonrecord_barrier(gcptr);  /* debugging: read barrier, but
                                         not recording anything */
int _stm_is_private(gcptr);  /* debugging */
gcptr stm_get_private_from_protected(long);  /* debugging */
gcptr stm_get_read_obj(long);  /* debugging */
void stm_clear_read_cache(void);  /* debugging */
gcptr stmgc_duplicate(gcptr);

int DescriptorInit(void);
void DescriptorDone(void);

#endif  /* _ET_H */
