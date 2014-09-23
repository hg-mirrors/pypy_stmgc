#define _STM_CORE_H_

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>


/************************************************************/

#ifndef STM_GC_NURSERY
# define STM_GC_NURSERY     4096          // 4MB
#endif


#define NB_PAGES            (2500*256)    // 2500MB
#define NB_SEGMENTS         STM_NB_SEGMENTS
#define NB_SEGMENTS_MAX     240    /* don't increase NB_SEGMENTS past this */
#define NB_NURSERY_PAGES    (STM_GC_NURSERY/4)

#define TOTAL_MEMORY          (NB_PAGES * 4096UL * NB_SEGMENTS)
#define READMARKER_END        ((NB_PAGES * 4096UL) >> 4)
#define FIRST_OBJECT_PAGE     ((READMARKER_END + 4095) / 4096UL)
#define FIRST_NURSERY_PAGE    FIRST_OBJECT_PAGE
#define END_NURSERY_PAGE      (FIRST_NURSERY_PAGE + NB_NURSERY_PAGES)
#define NB_SHARED_PAGES       (NB_PAGES - END_NURSERY_PAGE)

#define READMARKER_START      ((FIRST_OBJECT_PAGE * 4096UL) >> 4)
#define FIRST_READMARKER_PAGE (READMARKER_START / 4096UL)
#define OLD_RM_START          ((END_NURSERY_PAGE * 4096UL) >> 4)
#define FIRST_OLD_RM_PAGE     (OLD_RM_START / 4096UL)
#define NB_READMARKER_PAGES   (FIRST_OBJECT_PAGE - FIRST_READMARKER_PAGE)

enum /* stm_flags */ {
    GCFLAG_WRITE_BARRIER = _STM_GCFLAG_WRITE_BARRIER,
    GCFLAG_HAS_SHADOW = 0x02,
};


/************************************************************/


#define STM_PSEGMENT          ((stm_priv_segment_info_t *)STM_SEGMENT)

typedef TLPREFIX struct stm_priv_segment_info_s stm_priv_segment_info_t;

struct stm_priv_segment_info_s {
    struct stm_segment_info_s pub;

    uint8_t modified_objs_lock;

    /* All the old objects (older than the current transaction) that
       the current transaction attempts to modify.  This is used to
       track the STM status: these are old objects that where written
       to and that will need to be recorded in the commit log.  The
       list contains three entries for every such object, in the same
       format as 'struct stm_undo_s' below.
    */
    struct list_s *modified_old_objects;

    struct list_s *objects_pointing_to_nursery;
    struct tree_s *young_outside_nursery;
    struct tree_s *nursery_objects_shadows;

    uint8_t privatization_lock;

    uint8_t safe_point;
    uint8_t transaction_state;

    struct tree_s *callbacks_on_commit_and_abort[2];

    struct stm_commit_log_entry_s *last_commit_log_entry;

    struct stm_shadowentry_s *shadowstack_at_start_of_transaction;

    /* For debugging */
#ifndef NDEBUG
    pthread_t running_pthread;
#endif
};

enum /* safe_point */ {
    SP_NO_TRANSACTION=0,
    SP_RUNNING,
    SP_WAIT_FOR_C_REQUEST_REMOVED,
    SP_WAIT_FOR_C_AT_SAFE_POINT,
#ifdef STM_TESTS
    SP_WAIT_FOR_OTHER_THREAD,
#endif
};

enum /* transaction_state */ {
    TS_NONE=0,
    TS_REGULAR,
    TS_INEVITABLE,
};

/* Commit Log things */
struct stm_undo_s {
    object_t *object;   /* the object that is modified */
    char *backup;       /* some backup data (a slice of the original obj) */
    uint64_t slice;     /* location and size of this slice (== the whole
                           object, unless card marking is enabled).  The
                           size is in the lower 2 bytes, and the offset
                           in the remaining 6 bytes. */
};
#define SLICE_OFFSET(slice)  ((slice) >> 16)
#define SLICE_SIZE(slice)    ((int)((slice) & 0xFFFF))

struct stm_commit_log_entry_s {
    struct stm_commit_log_entry_s *volatile next;
    int segment_num;
    size_t written_count;
    struct stm_undo_s written[];
};
static struct stm_commit_log_entry_s commit_log_root = {NULL, -1, 0};


static char *stm_object_pages;
static char *stm_file_pages;
static int stm_object_pages_fd;
static stm_thread_local_t *stm_all_thread_locals = NULL;


#define REAL_ADDRESS(segment_base, src)   ((segment_base) + (uintptr_t)(src))


static inline char *get_segment_base(long segment_num) {
    return stm_object_pages + segment_num * (NB_PAGES * 4096UL);
}

static inline
struct stm_segment_info_s *get_segment(long segment_num) {
    return (struct stm_segment_info_s *)REAL_ADDRESS(
        get_segment_base(segment_num), STM_PSEGMENT);
}

static inline
struct stm_priv_segment_info_s *get_priv_segment(long segment_num) {
    return (struct stm_priv_segment_info_s *)REAL_ADDRESS(
        get_segment_base(segment_num), STM_PSEGMENT);
}

static inline int get_segment_of_linear_address(char *addr) {
    assert(addr > stm_object_pages && addr < stm_object_pages + TOTAL_MEMORY);
    return (addr - stm_object_pages) / (NB_PAGES * 4096UL);
}


static bool _is_tl_registered(stm_thread_local_t *tl);
static bool _seems_to_be_running_transaction(void);

static void abort_with_mutex(void) __attribute__((noreturn));
static stm_thread_local_t *abort_with_mutex_no_longjmp(void);
static void abort_data_structures_from_segment_num(int segment_num);

static void _signal_handler(int sig, siginfo_t *siginfo, void *context);
static void _stm_validate(void *free_if_abort);

static inline void _duck(void) {
    /* put a call to _duck() between two instructions that set 0 into
       a %gs-prefixed address and that may otherwise be replaced with
       llvm.memset --- it fails later because of the prefix...
       This is not needed any more after applying the patch
       llvmfix/no-memset-creation-with-addrspace.diff. */
    asm("/* workaround for llvm bug */");
}

static inline void acquire_privatization_lock(int segnum)
{
    spinlock_acquire(get_priv_segment(segnum)->privatization_lock);
}

static inline void release_privatization_lock(int segnum)
{
    spinlock_release(get_priv_segment(segnum)->privatization_lock);
}

static inline void acquire_modified_objs_lock(int segnum)
{
    spinlock_acquire(get_priv_segment(segnum)->modified_objs_lock);
}

static inline void release_modified_objs_lock(int segnum)
{
    spinlock_release(get_priv_segment(segnum)->modified_objs_lock);
}


static inline bool all_privatization_locks_acquired()
{
#ifndef NDEBUG
    long l;
    for (l = 0; l < NB_SEGMENTS; l++) {
        if (!get_priv_segment(l)->privatization_lock)
            return false;
    }
    return true;
#else
    abort();
#endif
}

static inline void acquire_all_privatization_locks()
{
    long l;
    for (l = 0; l < NB_SEGMENTS; l++) {
        acquire_privatization_lock(l);
    }
}

static inline void release_all_privatization_locks()
{
    long l;
    for (l = NB_SEGMENTS-1; l >= 0; l--) {
        release_privatization_lock(l);
    }
}
