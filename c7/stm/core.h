#define _STM_CORE_H_

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <errno.h>

/************************************************************/


#define NB_PAGES            (1500*256)    // 1500MB
#define NB_SEGMENTS         2
#define MAP_PAGES_FLAGS     (MAP_SHARED | MAP_ANONYMOUS | MAP_NORESERVE)
#define LARGE_OBJECT_WORDS  36
#define NB_NURSERY_PAGES    1024          // 4MB

#define TOTAL_MEMORY          (NB_PAGES * 4096UL * NB_SEGMENTS)
#define READMARKER_END        ((NB_PAGES * 4096UL) >> 4)
#define FIRST_OBJECT_PAGE     ((READMARKER_END + 4095) / 4096UL)
#define FIRST_NURSERY_PAGE    FIRST_OBJECT_PAGE
#define END_NURSERY_PAGE      (FIRST_NURSERY_PAGE + NB_NURSERY_PAGES)

#define READMARKER_START      ((FIRST_OBJECT_PAGE * 4096UL) >> 4)
#define FIRST_READMARKER_PAGE (READMARKER_START / 4096UL)
#define NB_READMARKER_PAGES   (FIRST_OBJECT_PAGE - FIRST_READMARKER_PAGE)

#define CREATMARKER_START     ((FIRST_OBJECT_PAGE * 4096UL) >> 8)
#define FIRST_CREATMARKER_PAGE (CREATMARKER_START / 4096UL)


enum {
    /* this flag is not set on most objects.  when stm_write() is called
       on an object that is not from the current transaction, then
       _stm_write_slowpath() is called, and then the flag is set to
       say "called once already, no need to call again". */
    GCFLAG_WRITE_BARRIER_CALLED = _STM_GCFLAG_WRITE_BARRIER_CALLED,
    /* allocated by gcpage.c in uniformly-sized pages of small objects */
    GCFLAG_SMALL_UNIFORM = 0x02,
};

#define CROSS_PAGE_BOUNDARY(start, stop)                                \
    (((uintptr_t)(start)) / 4096UL != ((uintptr_t)(stop)) / 4096UL)


/************************************************************/


#define STM_PSEGMENT          ((stm_priv_segment_info_t *)STM_SEGMENT)

typedef TLPREFIX struct stm_priv_segment_info_s stm_priv_segment_info_t;

struct stm_priv_segment_info_s {
    struct stm_segment_info_s pub;
    struct list_s *old_objects_to_trace;
    struct list_s *modified_objects;
    struct list_s *creation_markers;
    uint64_t start_time;
    uint8_t write_lock_num;
    uint8_t safe_point;         /* one of the SP_xxx constants */
    uint8_t transaction_state;  /* one of the TS_xxx constants */
    uint8_t min_read_version_outside_nursery;   /* see was_read_remote() */
    uintptr_t real_nursery_section_end;
    object_t **shadowstack_at_start_of_transaction;
};

enum {
    SP_NO_TRANSACTION=0,
    SP_RUNNING,
    SP_SAFE_POINT_CANNOT_COLLECT,
    SP_SAFE_POINT_CAN_COLLECT,
};
enum {
    TS_NONE=0,
    TS_REGULAR,
    TS_INEVITABLE,
    TS_MUST_ABORT,
};
enum {   /* for stm_creation_marker_t */
    CM_NOT_CURRENT_TRANSACTION             = 0x00,
    CM_CURRENT_TRANSACTION_OUTSIDE_NURSERY = 0x01,
    CM_CURRENT_TRANSACTION_IN_NURSERY      = 0xff,
};

static char *stm_object_pages;
static stm_thread_local_t *stm_thread_locals = NULL;

#ifdef STM_TESTS
static char *stm_other_pages;
#endif

static uint8_t write_locks[READMARKER_END - READMARKER_START];


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
static bool _running_transaction(void);

static inline bool obj_from_same_transaction(object_t *obj) {
    return ((stm_creation_marker_t *)(((uintptr_t)obj) >> 8))->cm !=
        CM_NOT_CURRENT_TRANSACTION;
}

static void teardown_core(void);
static void abort_with_mutex(void) __attribute__((noreturn));

static inline void _duck(void) {
    /* put a call to _duck() between two instructions that set 0 into
       a %gs-prefixed address and that may otherwise be replaced with
       llvm.memset --- it fails later because of the prefix...
       This is not needed any more after applying the patch
       llvmfix/no-memset-creation-with-addrspace.diff. */
    asm("/* workaround for llvm bug */");
}
