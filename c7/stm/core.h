
#define NB_PAGES            (1500*256)    // 1500MB
#define NB_THREADS          2
#define MAP_PAGES_FLAGS     (MAP_SHARED | MAP_ANONYMOUS | MAP_NORESERVE)
#define LARGE_OBJECT_WORDS  36
#define NB_NURSERY_PAGES    1024          // 4MB

#define NURSERY_SECTION_SIZE  (24*4096)


#define TOTAL_MEMORY          (NB_PAGES * 4096UL * NB_THREADS)
#define READMARKER_END        ((NB_PAGES * 4096UL) >> 4)
#define START_OBJECT_PAGE     ((READMARKER_END + 4095) / 4096UL)
#define START_NURSERY_PAGE    START_OBJECT_PAGE
#define READMARKER_START      ((START_OBJECT_PAGE * 4096UL) >> 4)
#define START_READMARKER_PAGE (READMARKER_START / 4096UL)
#define STOP_NURSERY_PAGE     (START_NURSERY_PAGE + NB_NURSERY_PAGES)


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


#define REAL_ADDRESS(thread_base, src)   ((thread_base) + (uintptr_t)(src))
