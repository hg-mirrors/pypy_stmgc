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

static uint64_t requested_minor_collections = 0;
static uint64_t completed_minor_collections = 0;


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


/************************************************************/


static void minor_collection(void)
{
    fprintf(stderr, "minor_collection\n");
    abort(); //...;

    assert(requested_minor_collections == completed_minor_collections + 1);
    completed_minor_collections += 1;
    nursery_ctl.used = 0;
}


static void sync_point_for_collection(void)
{
    mutex_lock();

    STM_PSEGMENT->safe_point = SP_SAFE_POINT_CAN_COLLECT;

 restart:
    if (requested_minor_collections == completed_minor_collections) {
        if (nursery_ctl.used < NURSERY_SIZE)
            goto exit;

        requested_minor_collections++;
    }

    /* are all threads in a safe-point? */
    long i;
    bool must_wait = false;
    for (i = 0; i < NB_SEGMENTS; i++) {
        struct stm_priv_segment_info_s *other_pseg = get_priv_segment(i);

        if (other_pseg->safe_point != SP_NO_TRANSACTION &&
            other_pseg->safe_point != SP_SAFE_POINT_CAN_COLLECT) {
            /* segment i is not at a safe point, or at one where
               collection is not possible (SP_SAFE_POINT_CANNOT_COLLECT) */

            /* we have the mutex here */
            other_pseg->pub.nursery_section_end = NSE_SIGNAL;
            must_wait = true;
        }
    }
    if (must_wait) {
        /* wait until all threads are indeed in a safe-point that allows
           collection */
        cond_wait();
        goto restart;
    }

    /* now we can run minor collection */
    minor_collection();

 exit:
    /* we have the mutex here, and at this point there is no
       pending requested minor collection, so we simply reset
       our value of nursery_section_end and return. */
    STM_SEGMENT->nursery_section_end =
        STM_PSEGMENT->real_nursery_section_end;

    STM_PSEGMENT->safe_point = SP_RUNNING;

    mutex_unlock();
}


/************************************************************/

#define NURSERY_ALIGN(bytes)  \
    (((bytes) + NURSERY_LINE - 1) & ~(NURSERY_LINE - 1))

static stm_char *allocate_from_nursery(uint64_t bytes)
{
    /* may collect! */
    /* thread-safe; allocate a chunk of memory from the nursery */
    bytes = NURSERY_ALIGN(bytes);
    while (1) {
        uint64_t p = __sync_fetch_and_add(&nursery_ctl.used, bytes);
        if (LIKELY(p + bytes <= NURSERY_SIZE)) {
            return (stm_char *)(NURSERY_START + p);
        }
        sync_point_for_collection();
    }
}


stm_char *_stm_allocate_slowpath(ssize_t size_rounded_up)
{
    /* may collect! */
    STM_SEGMENT->nursery_current -= size_rounded_up;  /* restore correct val */

 restart:
    if (UNLIKELY(STM_SEGMENT->nursery_section_end == NSE_SIGNAL)) {

        /* If nursery_section_end was set to NSE_SIGNAL by another thread,
           we end up here as soon as we try to call stm_allocate(). */
        sync_point_for_collection();

        /* Once the sync point is done, retry. */
        goto restart;
    }

    if (size_rounded_up < MEDIUM_OBJECT) {
        /* This is a small object.  We first try to check if the current
           section really doesn't fit the object; maybe all we were called
           for was the sync point above */
        stm_char *p1 = STM_SEGMENT->nursery_current;
        stm_char *end1 = p1 + size_rounded_up;
        if ((uintptr_t)end1 <= STM_PSEGMENT->real_nursery_section_end) {
            /* fits */
            STM_SEGMENT->nursery_current = end1;
            return p1;
        }

        /* Otherwise, the current section is really full.
           Allocate the next section and initialize it with zeroes. */
        stm_char *p = allocate_from_nursery(NURSERY_SECTION_SIZE);
        STM_SEGMENT->nursery_current = p + size_rounded_up;

        /* Set nursery_section_end, but carefully: another thread may
           have forced it to be equal to NSE_SIGNAL. */
        uintptr_t end = (uintptr_t)p + NURSERY_SECTION_SIZE;

        if (UNLIKELY(!__sync_bool_compare_and_swap(
               &STM_SEGMENT->nursery_section_end,
               STM_PSEGMENT->real_nursery_section_end,
               end))) {
            assert(STM_SEGMENT->nursery_section_end == NSE_SIGNAL);
            goto restart;
        }

        STM_PSEGMENT->real_nursery_section_end = end;

        memset(REAL_ADDRESS(STM_SEGMENT->segment_base, p), 0,
               NURSERY_SECTION_SIZE);

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

    uint64_t size = STM_PSEGMENT->real_nursery_section_end - c;
    if (size > 0) {
        set_creation_markers((stm_char *)c, size,
                             CM_CURRENT_TRANSACTION_IN_NURSERY);
    }
}

#ifdef STM_TESTS
void _stm_set_nursery_free_count(uint64_t free_count)
{
    assert(free_count == NURSERY_ALIGN(free_count));
    assert(nursery_ctl.used <= NURSERY_SIZE - free_count);
    nursery_ctl.used = NURSERY_SIZE - free_count;
}
#endif
