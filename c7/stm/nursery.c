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
#define MEDIUM_OBJECT         (6*1024)

/* size in bytes of the "line".  Should be equal to the line used by
   stm_creation_marker_t. */
#define NURSERY_LINE          256

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
    assert(NURSERY_LINE == (1 << 8));  /* from stm_creation_marker_t */
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
        set_creation_markers(p, NURSERY_SECTION_SIZE,
                             CM_CURRENT_TRANSACTION_IN_NURSERY);
        return p;
    }

    if (size_rounded_up < LARGE_OBJECT) {
        /* A medium-sized object that doesn't fit into the current
           nursery section.  Note that if by chance it does fit, then
           _stm_allocate_slowpath() is not even called.  This case here
           is to prevent too much of the nursery to remain not used
           just because we tried to allocate a medium-sized object:
           doing so doesn't end the current section. */
        stm_char *p = allocate_from_nursery(size_rounded_up);
        memset(REAL_ADDRESS(STM_SEGMENT->segment_base, p), 0,
               size_rounded_up);
        set_single_creation_marker(p, CM_CURRENT_TRANSACTION_IN_NURSERY);
        return p;
    }

    abort();
}

static void align_nursery_at_transaction_start(void)
{
    /* When the transaction start, we must align the 'nursery_current'
       and set creation markers for the part of the section the follows.
    */
    uintptr_t c = (uintptr_t)STM_SEGMENT->nursery_current;
    c = NURSERY_ALIGN(c);
    STM_SEGMENT->nursery_current = (stm_char *)c;

    uint64_t size = STM_SEGMENT->nursery_section_end - c;
    if (size > 0) {
        set_creation_markers((stm_char *)c, size,
                             CM_CURRENT_TRANSACTION_IN_NURSERY);
    }
}
