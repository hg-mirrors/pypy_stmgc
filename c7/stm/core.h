#define _STM_CORE_H_

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>


#define NB_PAGES            (1500*256)    // 1500MB
#define NB_REGIONS          2
#define MAP_PAGES_FLAGS     (MAP_SHARED | MAP_ANONYMOUS | MAP_NORESERVE)
#define LARGE_OBJECT_WORDS  36
#define NB_NURSERY_PAGES    1024          // 4MB

#define NURSERY_SECTION_SIZE  (24*4096)


#define TOTAL_MEMORY          (NB_PAGES * 4096UL * NB_REGIONS)
#define READMARKER_END        ((NB_PAGES * 4096UL) >> 4)
#define FIRST_OBJECT_PAGE     ((READMARKER_END + 4095) / 4096UL)
#define FIRST_NURSERY_PAGE    FIRST_OBJECT_PAGE
#define READMARKER_START      ((FIRST_OBJECT_PAGE * 4096UL) >> 4)
#define FIRST_READMARKER_PAGE (READMARKER_START / 4096UL)
#define END_NURSERY_PAGE      (FIRST_NURSERY_PAGE + NB_NURSERY_PAGES)


enum {
    /* set if the write-barrier slowpath needs to trigger. set on all
       old objects if there was no write-barrier on it in the same
       transaction and no collection inbetween. */
    GCFLAG_WRITE_BARRIER = _STM_GCFLAG_WRITE_BARRIER,
    /* set on objects which are in pages visible to others (SHARED
       or PRIVATE), but not committed yet. So only visible from
       this transaction. */
    //GCFLAG_NOT_COMMITTED = _STM_GCFLAG_WRITE_BARRIER << 1,
    /* only used during collections to mark an obj as moved out of the
       generation it was in */
    //GCFLAG_MOVED = _STM_GCFLAG_WRITE_BARRIER << 2,
    /* objects smaller than one page and even smaller than
       LARGE_OBJECT_WORDS * 8 bytes */
    //GCFLAG_SMALL = _STM_GCFLAG_WRITE_BARRIER << 3,
};


#define STM_PREGION          ((stm_priv_region_info_t *)STM_REGION)

typedef TLPREFIX struct stm_priv_region_info_s stm_priv_region_info_t;

struct stm_priv_region_info_s {
    struct stm_region_info_s pub;
};

static char *stm_object_pages;
static stm_thread_local_t *stm_thread_locals = NULL;


#define REAL_ADDRESS(region_base, src)   ((region_base) + (uintptr_t)(src))

static inline char *get_region_base(long region_num) {
    return stm_object_pages + region_num * (NB_PAGES * 4096UL);
}

static inline struct stm_region_info_s *get_region(long region_num) {
    return (struct stm_region_info_s *)REAL_ADDRESS(
        get_region_base(region_num), STM_PREGION);
}

static inline struct stm_priv_region_info_s *get_priv_region(long region_num) {
    return (struct stm_priv_region_info_s *)REAL_ADDRESS(
        get_region_base(region_num), STM_PREGION);
}
