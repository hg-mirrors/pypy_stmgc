#ifndef _STM_CORE_H_
# error "must be compiled via stmgc.c"
#endif

/************************************************************/

#define NURSERY_START         (FIRST_NURSERY_PAGE * 4096UL)
#define NURSERY_SIZE          (NB_NURSERY_PAGES * 4096UL)

/* an object larger than LARGE_OBJECT will never be allocated in
   the nursery. */
#define LARGE_OBJECT          (65*1024)

/* the nursery is divided in "sections" this big.  Each section is
   allocated to a single running thread. */
#define NURSERY_SECTION_SIZE  (128*1024)

/* if objects are larger than this limit but smaller than LARGE_OBJECT,
   then they might be allocted outside sections but still in the nursery. */
#define MEDIUM_OBJECT         (8*1024)

/* size in bytes of the "line".  Should be equal to the line used by
   stm_creation_marker_t. */
#define NURSERY_LINE_SHIFT    8
#define NURSERY_LINE          (1 << NURSERY_LINE_SHIFT)

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
    assert((NURSERY_SECTION_SIZE % NURSERY_LINE) == 0);
    assert(MEDIUM_OBJECT < LARGE_OBJECT);
    assert(LARGE_OBJECT < NURSERY_SECTION_SIZE);
    nursery_ctl.used = 0;
}

bool _stm_in_nursery(object_t *obj)
{
    assert((uintptr_t)obj >= NURSERY_START);
    return (uintptr_t)obj < NURSERY_START + NURSERY_SIZE;
}

static void set_creation_markers(stm_char *p, uint64_t size)
{
    /* Set the creation markers to 0xff for all lines from p to p+size.
       Both p and size should be aligned to NURSERY_LINE. */

    assert((((uintptr_t)p) & (NURSERY_LINE - 1)) == 0);
    assert((size & (NURSERY_LINE - 1)) == 0);

    char *addr = REAL_ADDRESS(STM_SEGMENT->segment_base,
                              ((uintptr_t)p) >> NURSERY_LINE_SHIFT);
    memset(addr, 0xff, size >> NURSERY_LINE_SHIFT);

    LIST_APPEND(STM_PSEGMENT->creation_markers, addr);
}

static void reset_all_creation_markers(void)
{
    /* Note that the page 'NB_PAGES - 1' is not actually used.  This
       ensures that the creation markers always end with some zeroes.
       We reset the markers 8 at a time, by writing null integers
       until we reach a place that is already null.
    */
    LIST_FOREACH_R(
        STM_PSEGMENT->creation_markers,
        uintptr_t /*item*/,
        ({
            uint64_t *p = (uint64_t *)(item & ~7);
            while (*p != 0)
                *p++ = 0;
        }));

    list_clear(STM_PSEGMENT->creation_markers);
}


#define NURSERY_ALIGN(bytes)  \
    (((bytes) + NURSERY_LINE - 1) & ~(NURSERY_LINE - 1))

static stm_char *allocate_from_nursery(uint64_t bytes)
{
    /* thread-safe; allocate a chunk of memory from the nursery */
    bytes = NURSERY_ALIGN(bytes);
    uint64_t p = __sync_fetch_and_add(&nursery_ctl.used, bytes);
    if (p + bytes > NURSERY_SIZE) {
        //major_collection();
        abort();
    }
    return (stm_char *)(NURSERY_START + p);
}


stm_char *_stm_allocate_slowpath(ssize_t size_rounded_up)
{
    if (size_rounded_up < MEDIUM_OBJECT) {
        /* This is a small object.  The current section is simply full.
           Allocate the next section and initialize it with zeroes. */
        stm_char *p = allocate_from_nursery(NURSERY_SECTION_SIZE);
        memset(REAL_ADDRESS(STM_SEGMENT->segment_base, p), 0,
               NURSERY_SECTION_SIZE);
        STM_SEGMENT->nursery_current = p + size_rounded_up;
        STM_SEGMENT->nursery_section_end = (uintptr_t)p + NURSERY_SECTION_SIZE;

        /* Also fill the corresponding creation markers with 0xff. */
        set_creation_markers(p, NURSERY_SECTION_SIZE);

        return p;
    }
    abort();
}
