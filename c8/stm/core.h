#define _STM_CORE_H_

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <errno.h>
#include <pthread.h>

/************************************************************/

#ifndef STM_GC_NURSERY
# define STM_GC_NURSERY     4096          // 4MB
#endif


#define NB_PAGES            (2500*256)    // 2500MB
#define NB_SEGMENTS         STM_NB_SEGMENTS
#define NB_SEGMENTS_MAX     240    /* don't increase NB_SEGMENTS past this */
#define MAP_PAGES_FLAGS     (MAP_SHARED | MAP_ANONYMOUS | MAP_NORESERVE)
#define NB_NURSERY_PAGES    (STM_GC_NURSERY/4)

#define TOTAL_MEMORY          (NB_PAGES * 4096UL * (1 + NB_SEGMENTS))
#define READMARKER_END        ((NB_PAGES * 4096UL) >> 4)
#define FIRST_OBJECT_PAGE     ((READMARKER_END + 4095) / 4096UL)
#define FIRST_NURSERY_PAGE    FIRST_OBJECT_PAGE
#define END_NURSERY_PAGE      (FIRST_NURSERY_PAGE + NB_NURSERY_PAGES)

#define READMARKER_START      ((FIRST_OBJECT_PAGE * 4096UL) >> 4)
#define FIRST_READMARKER_PAGE (READMARKER_START / 4096UL)
#define OLD_RM_START          ((END_NURSERY_PAGE * 4096UL) >> 4)
#define FIRST_OLD_RM_PAGE     (OLD_RM_START / 4096UL)
#define NB_READMARKER_PAGES   (FIRST_OBJECT_PAGE - FIRST_READMARKER_PAGE)


enum /* stm_flags */ {
    GCFLAG_WRITE_BARRIER = _STM_GCFLAG_WRITE_BARRIER,
};


/************************************************************/


#define STM_PSEGMENT          ((stm_priv_segment_info_t *)STM_SEGMENT)

typedef TLPREFIX struct stm_priv_segment_info_s stm_priv_segment_info_t;

struct stm_priv_segment_info_s {
    struct stm_segment_info_s pub;

    struct list_s *modified_old_objects;
    struct list_s *objects_pointing_to_nursery;
    uint8_t privatization_lock;

    struct stm_commit_log_entry_s *last_commit_log_entry;

    /* For debugging */
#ifndef NDEBUG
    pthread_t running_pthread;
#endif
};

/* Commit Log things */
struct stm_commit_log_entry_s {
    struct stm_commit_log_entry_s *next;
    int segment_num;
    object_t *written[];        /* terminated with a NULL ptr */
};
static struct stm_commit_log_entry_s commit_log_root = {NULL, -1};


static char *stm_object_pages;
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

static bool _is_tl_registered(stm_thread_local_t *tl);
static bool _seems_to_be_running_transaction(void);


static inline void _duck(void) {
    /* put a call to _duck() between two instructions that set 0 into
       a %gs-prefixed address and that may otherwise be replaced with
       llvm.memset --- it fails later because of the prefix...
       This is not needed any more after applying the patch
       llvmfix/no-memset-creation-with-addrspace.diff. */
    asm("/* workaround for llvm bug */");
}

static inline void acquire_privatization_lock(void)
{
    uint8_t *lock = (uint8_t *)REAL_ADDRESS(STM_SEGMENT->segment_base,
                                            &STM_PSEGMENT->privatization_lock);
    spinlock_acquire(*lock);
}

static inline void release_privatization_lock(void)
{
    uint8_t *lock = (uint8_t *)REAL_ADDRESS(STM_SEGMENT->segment_base,
                                            &STM_PSEGMENT->privatization_lock);
    spinlock_release(*lock);
}
