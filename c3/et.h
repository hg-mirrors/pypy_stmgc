/*** Extendable Timestamps
 *
 * Documentation:
 * https://bitbucket.org/pypy/extradoc/raw/extradoc/talk/stm2012/stmimpl.rst
 *
 * This is very indirectly based on rstm_r5/stm/et.hpp.
 * See http://www.cs.rochester.edu/research/synchronization/rstm/api.shtml
 */

#ifndef _SRCSTM_ET_H
#define _SRCSTM_ET_H


#define LOCKED  ((INTPTR_MAX - 0xffff) | 1)

/* Description of the flags
 * ------------------------
 *
 * Objects are either "young" or "old" depending on the generational garbage
 * collection: "young" objects are the ones in the nursery (plus a few big
 * ones outside) and will be collected by the following minor collection.
 *
 * Additionally, objects are either "public", "protected" or "private".  The
 * private objects have h_revision == stm_local_revision and are invisible
 * to other threads.  They become non-private when the transaction commits.
 *
 *                          non-private | private
 *           +------------------------------------------------------------
 *           |
 *       old |         public objects   |   old private objects
 *  ---------|
 *           |
 *     young |     [ protected objects  |  private objects  (--> grows) ]
 *  (nursery)|
 *
 * GCFLAG_PRIVATE_COPY is set on a private object that is a "private copy",
 * i.e. is the newer version of some pre-existing non-private object.
 *
 * GCFLAG_VISITED is used temporarily during major collections.
 *
 * GCFLAG_PUBLIC_TO_PRIVATE is added to a *public* object that has got a
 * *private* copy.  It is sticky, reset only at the next major collection.
 *
 * GCFLAG_PREBUILT_ORIGINAL is only set on the original version of
 * prebuilt objects.
 *
 * GCFLAG_WRITE_BARRIER is set on *old* *private* objects to track old-to-
 * young pointers.  It may be left set on *public* objects but is ignored
 * there, because the write barrier will trigger anyway on any non-private
 * object.  On an old private object, it is removed once a write occurs
 * and the object is recorded in 'private_old_pointing_to_young'; it is
 * set again at the next minor collection.
 *
 * GCFLAG_NURSERY_MOVED is used temporarily during minor collections.
 */
#define GCFLAG_PRIVATE_COPY      (STM_FIRST_GCFLAG << 0)
#define GCFLAG_VISITED           (STM_FIRST_GCFLAG << 1)
#define GCFLAG_PUBLIC_TO_PRIVATE (STM_FIRST_GCFLAG << 2)
#define GCFLAG_PREBUILT_ORIGINAL (STM_FIRST_GCFLAG << 3)
#define GCFLAG_WRITE_BARRIER     (STM_FIRST_GCFLAG << 4)
#define GCFLAG_NURSERY_MOVED     (STM_FIRST_GCFLAG << 5)

/* this value must be reflected in PREBUILT_FLAGS in stmgc.h */
#define GCFLAG_PREBUILT  (GCFLAG_VISITED           | \
                          GCFLAG_PREBUILT_ORIGINAL)

#define GC_FLAG_NAMES  { "PRIVATE_COPY",      \
                         "VISITED",           \
                         "PUBLIC_TO_PRIVATE", \
                         "PREBUILT_ORIGINAL", \
                         "WRITE_BARRIER",     \
                         "NURSERY_MOVED",     \
                         NULL }

/************************************************************/

#define ABRT_MANUAL               0
#define ABRT_COMMIT               1
#define ABRT_VALIDATE_INFLIGHT    2
#define ABRT_VALIDATE_COMMIT      3
#define ABRT_VALIDATE_INEV        4
#define ABRT_COLLECT_MINOR        5
#define ABRT_COLLECT_MAJOR        6
#define ABORT_REASONS         7

#define SPLP_ABORT                0
#define SPLP_LOCKED_INFLIGHT      1
#define SPLP_LOCKED_VALIDATE      2
#define SPLP_LOCKED_COMMIT        3
#define SPINLOOP_REASONS      4

struct tx_descriptor {
  NURSERY_FIELDS_DECL
  local_gcpages_t *local_gcpages;
  jmp_buf *setjmp_buf;
  revision_t start_time;
  revision_t my_lock;
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
  struct GcPtrList abortinfo;
  char *longest_abort_info;
  long long longest_abort_info_time;
  struct FXCache recent_reads_cache;
  revision_t *local_revision_ref;
  struct tx_descriptor *tx_next, *tx_prev;   /* a doubly linked list */
};

extern __thread struct tx_descriptor *thread_descriptor;
extern __thread revision_t stm_local_revision;

/************************************************************/

//#define STM_BARRIER_P2R(P)
//    (__builtin_expect((((gcptr)(P))->h_tid & GCFLAG_GLOBAL) == 0, 1) ?
//     (P) : (typeof(P))stm_DirectReadBarrier(P))

//#define STM_BARRIER_G2R(G)
//    (assert(((gcptr)(G))->h_tid & GCFLAG_GLOBAL),
//     (typeof(G))stm_DirectReadBarrier(G))

//#define STM_BARRIER_O2R(O)
//    (__builtin_expect((((gcptr)(O))->h_tid & GCFLAG_POSSIBLY_OUTDATED) == 0,
//                      1) ?
//     (O) : (typeof(O))stm_RepeatReadBarrier(O))

//#define STM_READ_BARRIER_P_FROM_R(P, R_container, offset)
//    (__builtin_expect((((gcptr)(P))->h_tid & GCFLAG_GLOBAL) == 0, 1) ?
//     (P) : (typeof(P))stm_DirectReadBarrierFromR((P),
//                                              (R_container),
//                                              offset))

//#define STM_BARRIER_P2W(P)
//    (__builtin_expect((((gcptr)(P))->h_tid & GCFLAG_NOT_WRITTEN) == 0, 1) ?
//     (P) : (typeof(P))stm_WriteBarrier(P))

//#define STM_BARRIER_G2W(G)
//    (assert(((gcptr)(G))->h_tid & GCFLAG_GLOBAL),
//     (typeof(G))stm_WriteBarrier(G))

//#define STM_BARRIER_R2W(R)
//    (__builtin_expect((((gcptr)(R))->h_tid & GCFLAG_NOT_WRITTEN) == 0, 1) ?
//     (R) : (typeof(R))stm_WriteBarrierFromReady(R))

//#define STM_BARRIER_O2W(R)  STM_BARRIER_R2W(R)   /* same logic works here */


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
gcptr _stm_nonrecord_barrier(gcptr, int *);

int DescriptorInit(void);
void DescriptorDone(void);

#endif  /* _ET_H */
