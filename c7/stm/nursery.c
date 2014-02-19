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

static bool _is_young(object_t *obj)
{
    return _stm_in_nursery(obj);    /* for now */
}


/************************************************************/

#define GCWORD_MOVED  ((object_t *) -42)


static inline void minor_copy_in_page_to_other_segments(uintptr_t p,
                                                        size_t size)
{
    uintptr_t dataofs = (char *)p - stm_object_pages;
    assert((dataofs & 4095) + size <= 4096);   /* fits in one page */

    if (flag_page_private[dataofs / 4096UL] != SHARED_PAGE) {
        long i;
        for (i = 1; i < NB_SEGMENTS; i++) {
            memcpy(get_segment_base(i) + dataofs, (char *)p, size);
        }
    }
}


static void minor_trace_if_young(object_t **pobj)
{
    /* takes a normal pointer to a thread-local pointer to an object */
    object_t *obj = *pobj;
    if (obj == NULL)
        return;
    if (!_is_young(obj))
        return;

    /* the location the object moved to is the second word in 'obj' */
    char *realobj = (char *)REAL_ADDRESS(stm_object_pages, obj);
    object_t **pforwarded_array = (object_t **)realobj;

    if (pforwarded_array[0] == GCWORD_MOVED) {
        *pobj = pforwarded_array[1];    /* already moved */
        return;
    }

    /* We need to make a copy of this object.  There are three different
       places where the copy can be located, based on four criteria.

       1) object larger than GC_MEDIUM_REQUEST        => largemalloc.c
       2) otherwise, object from current transaction  => page S
       3) otherwise, object with the write lock       => page W
       4) otherwise, object without the write lock    => page S

       The pages S or W above are both pages of uniform sizes obtained
       from the end of the address space.  The difference is that page S
       can be shared, but page W needs to be privatized.
    */
    size_t size = stmcb_size_rounded_up((struct object_s *)realobj);
    uintptr_t lock_idx = (((uintptr_t)obj) >> 4) - READMARKER_START;
    uint8_t write_lock = write_locks[lock_idx];
    object_t *nobj;
    long i;

    if (1 /*size >= GC_MEDIUM_REQUEST*/) {

        /* case 1: object is larger than GC_MEDIUM_REQUEST.
           Ask gcpage.c for an allocation via largemalloc. */
        char *copyobj;
        copyobj = allocate_outside_nursery_large(size < 256 ? 256 : size);  // XXX temp

        /* Copy the object to segment 0 (as a first step) */
        memcpy(copyobj, realobj, size);

        nobj = (object_t *)(copyobj - stm_object_pages);

        if (write_lock == 0) {
            /* The object is not write-locked, so any copy should be
               identical.  Now some pages of the destination might be
               private already (because of some older action); if so, we
               need to replicate the corresponding parts.  The hope is
               that it's relatively uncommon. */
            uintptr_t p, pend = ((uintptr_t)(copyobj + size - 1)) & ~4095;
            for (p = (uintptr_t)copyobj; p < pend; p = (p + 4096) & ~4095) {
                minor_copy_in_page_to_other_segments(p, 4096 - (p & 4095));
            }
            minor_copy_in_page_to_other_segments(p, ((size-1) & 4095) + 1);
        }
        else {
            /* The object has the write lock.  We need to privatize the
               pages, and repeat the write lock in the new copy. */
            uintptr_t dataofs = (uintptr_t)nobj;
            uintptr_t pagenum = dataofs / 4096UL;
            uintptr_t lastpage= (dataofs + size - 1) / 4096UL;
            pages_privatize(pagenum, lastpage - pagenum + 1, false);

            lock_idx = (dataofs >> 4) - READMARKER_START;
            assert(write_locks[lock_idx] == 0);
            write_locks[lock_idx] = write_lock;

            /* Then, for each segment > 0, we need to repeat the
               memcpy() done above.  XXX This could be optimized if
               NB_SEGMENTS > 2 by reading all non-written copies from the
               same segment, instead of reading really all segments. */
            for (i = 1; i < NB_SEGMENTS; i++) {
                uintptr_t diff = get_segment_base(i) - stm_object_pages;
                memcpy(copyobj + diff, realobj + diff, size);
            }
        }

        /* If the source creation marker is CM_CURRENT_TRANSACTION_IN_NURSERY,
           write CM_CURRENT_TRANSACTION_OUTSIDE_NURSERY in the destination */
        uintptr_t cmaddr = ((uintptr_t)obj) >> 8;

        for (i = 0; i < NB_SEGMENTS; i++) {
            char *absaddr = get_segment_base(i) + cmaddr;
            if (((struct stm_creation_marker_s *)absaddr)->cm != 0) {
                uintptr_t ncmaddr = ((uintptr_t)nobj) >> 8;
                absaddr = get_segment_base(i) + ncmaddr;
                ((struct stm_creation_marker_s *)absaddr)->cm =
                    CM_CURRENT_TRANSACTION_OUTSIDE_NURSERY;
            }
        }
    }
    else {
        /* cases 2 to 4 */
        abort();  //...
        allocate_outside_nursery_small(small_alloc_shared, size);
        allocate_outside_nursery_small(small_alloc_privtz, size);
    }

    /* Copy the read markers */
    for (i = 0; i < NB_SEGMENTS; i++) {
        uint8_t rm = get_segment_base(i)[((uintptr_t)obj) >> 4];
        get_segment_base(i)[((uintptr_t)nobj) >> 4] = rm;
    }

    /* Done copying the object. */
    pforwarded_array[0] = GCWORD_MOVED;
    pforwarded_array[1] = nobj;
    *pobj = nobj;
}

static void minor_trace_roots(void)
{
    stm_thread_local_t *tl = stm_thread_locals;
    do {
        object_t **current = tl->shadowstack;
        object_t **base = tl->shadowstack_base;
        while (current-- != base) {
            minor_trace_if_young(current);
        }
        tl = tl->next;
    } while (tl != stm_thread_locals);
}

static void reset_all_nursery_section_ends(void)
{
    long i;
    for (i = 0; i < NB_SEGMENTS; i++) {
        struct stm_priv_segment_info_s *other_pseg = get_priv_segment(i);
        /* no race condition here, because all other threads are paused
           in safe points, so cannot be e.g. in _stm_allocate_slowpath() */
        other_pseg->real_nursery_section_end = 0;
        other_pseg->pub.v_nursery_section_end = 0;
    }
    nursery_ctl.used = 0;

    /* reset the write locks */
    memset(write_locks + ((NURSERY_START >> 4) - READMARKER_START),
           0, NURSERY_SIZE >> 4);

    /* reset the read markers, and the creation markers --
       maybe in each thread in parallel?? */
    abort();//XXX;...
}

static void do_minor_collection(void)
{
    /* all other threads are paused in safe points during the whole
       minor collection */
    assert(_has_mutex());

    check_gcpage_still_shared();

    minor_trace_roots();

    // copy modified_objects


    fprintf(stderr, "minor_collection\n");
    abort(); //...;


    reset_all_nursery_section_ends();
}


static void restore_nursery_section_end(uintptr_t prev_value)
{
    __sync_bool_compare_and_swap(&STM_SEGMENT->v_nursery_section_end,
                                 prev_value,
                                 STM_PSEGMENT->real_nursery_section_end);
}

static void stm_minor_collection(uint64_t request_size)
{
    /* Run a minor collection --- but only if we can't get 'request_size'
       bytes out of the nursery; if we can, no-op. */
    mutex_lock();

    assert(STM_PSEGMENT->safe_point == SP_RUNNING);
    STM_PSEGMENT->safe_point = SP_SAFE_POINT_CAN_COLLECT;

 restart:
    /* We just waited here, either from mutex_lock() or from cond_wait(),
       so we should check again if another thread did the minor
       collection itself */
    if (nursery_ctl.used + request_size <= NURSERY_SIZE)
        goto exit;

    if (!try_wait_for_other_safe_points(SP_SAFE_POINT_CAN_COLLECT))
        goto restart;

    /* now we can run our minor collection */
    do_minor_collection();

 exit:
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

        /* nursery full! */
        stm_minor_collection(bytes);
    }
}


stm_char *_stm_allocate_slowpath(ssize_t size_rounded_up)
{
    /* may collect! */
    STM_SEGMENT->nursery_current -= size_rounded_up;  /* restore correct val */

    if (_stm_collectable_safe_point())
        return (stm_char *)stm_allocate(size_rounded_up);

    if (size_rounded_up < MEDIUM_OBJECT) {
        /* This is a small object.  The current section is really full.
           Allocate the next section and initialize it with zeroes. */
        stm_char *p = allocate_from_nursery(NURSERY_SECTION_SIZE);
        STM_SEGMENT->nursery_current = p + size_rounded_up;

        /* Set v_nursery_section_end, but carefully: another thread may
           have forced it to be equal to NSE_SIGNAL. */
        uintptr_t end = (uintptr_t)p + NURSERY_SECTION_SIZE;
        uintptr_t prev_end = STM_PSEGMENT->real_nursery_section_end;
        STM_PSEGMENT->real_nursery_section_end = end;
        restore_nursery_section_end(prev_end);

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
    /* When the transaction starts, we must align the 'nursery_current'
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
