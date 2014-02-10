#ifndef _STM_CORE_H_
# error "must be compiled via stmgc.c"
#endif

/************************************************************/

#define NURSERY_SIZE          (NB_NURSERY_PAGES * 4096UL)

/* an object larger than LARGE_OBJECT will never be allocated in
   the nursery. */
#define LARGE_OBJECT          (65*1024)

/* the nursery is divided in "sections" this big.  Each section is
   allocated to a single running thread. */
#define NURSERY_SECTION_SIZE  (128*1024)

/* if objects are larger than this limit but smaller than LARGE_OBJECT,
   then they might be allocted outside sections but still in the nursery. */
#define MEDIUM_OBJECT         (9*1024)

/************************************************************/

static union {
    struct {
        uint64_t used;    /* number of bytes from the nursery used so far */
    };
    char reserved[64];
} nursery_ctl __attribute__((aligned(64)));

/************************************************************/

static void setup_nursery(void)
{
    assert(MEDIUM_OBJECT < LARGE_OBJECT);
    assert(LARGE_OBJECT < NURSERY_SECTION_SIZE);
    nursery_ctl.used = 0;
}


static stm_char *allocate_from_nursery(uint64_t bytes)
{
    /* thread-safe; allocate a chunk of memory from the nursery */
    uint64_t p = __sync_fetch_and_add(&nursery_ctl.used, bytes);
    if (p + bytes > NURSERY_SIZE) {
        //major_collection();
        abort();
    }
    return (stm_char *)(FIRST_NURSERY_PAGE * 4096UL + p);
}


stm_char *_stm_allocate_slowpath(ssize_t size_rounded_up)
{
    if (size_rounded_up < MEDIUM_OBJECT) {
        /* This is a small object.  The current section is simply full.
           Allocate the next section. */
        stm_char *p = allocate_from_nursery(NURSERY_SECTION_SIZE);
        STM_SEGMENT->nursery_current = p + size_rounded_up;
        STM_SEGMENT->nursery_section_end = (uintptr_t)p + NURSERY_SECTION_SIZE;
        return p;
    }
    abort();
}
