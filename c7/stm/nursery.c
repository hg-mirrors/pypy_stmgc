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

static struct list_s *old_objects_pointing_to_young;


/************************************************************/

static void setup_nursery(void)
{
    assert(NURSERY_LINE == (1 << 8));  /* from stm_creation_marker_t */
    assert((NURSERY_SECTION_SIZE % NURSERY_LINE) == 0);
    assert(MEDIUM_OBJECT < LARGE_OBJECT);
    assert(LARGE_OBJECT < NURSERY_SECTION_SIZE);
    nursery_ctl.used = 0;
    old_objects_pointing_to_young = list_create();
}

static void teardown_nursery(void)
{
    list_free(old_objects_pointing_to_young);
}

static inline bool _is_in_nursery(object_t *obj)
{
    assert((uintptr_t)obj >= NURSERY_START);
    return (uintptr_t)obj < NURSERY_START + NURSERY_SIZE;
}

bool _stm_in_nursery(object_t *obj)
{
    return _is_in_nursery(obj);
}

static bool _is_young(object_t *obj)
{
    return _is_in_nursery(obj);    /* for now */
}

static inline bool was_read_remote(char *base, object_t *obj,
                                   uint8_t other_transaction_read_version,
                                   uint8_t min_read_version_outside_nursery)
{
    uint8_t rm = ((struct stm_read_marker_s *)
                  (base + (((uintptr_t)obj) >> 4)))->rm;

    assert(min_read_version_outside_nursery <=
           other_transaction_read_version);
    assert(rm <= other_transaction_read_version);

    if (_is_in_nursery(obj)) {
        return rm == other_transaction_read_version;
    }
    else {
        return rm >= min_read_version_outside_nursery;
    }
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

    /* If the object was already seen here, its first word was set
       to GCWORD_MOVED.  In that case, the forwarding location, i.e.
       where the object moved to, is stored in the second word in 'obj'. */
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
       can be shared, but page W needs to be privatized.  Moreover,
       cases 2 and 4 differ in the creation_marker they need to put,
       which has a granularity of 256 bytes.
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

    /* Must trace the object later */
    LIST_APPEND(old_objects_pointing_to_young, nobj);
}

static void collect_roots_in_nursery(void)
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

static void trace_and_drag_out_of_nursery(object_t *obj)
{
    if (is_in_shared_pages(obj)) {
        /* the object needs fixing only in one copy, because all copies
           are shared and identical. */
        char *realobj = (char *)REAL_ADDRESS(stm_object_pages, obj);
        stmcb_trace((struct object_s *)realobj, &minor_trace_if_young);
    }
    else {
        /* every segment needs fixing */
        long i;
        for (i = 0; i < NB_SEGMENTS; i++) {
            char *realobj = (char *)REAL_ADDRESS(get_segment_base(i), obj);
            stmcb_trace((struct object_s *)realobj, &minor_trace_if_young);
        }
    }
}

static void collect_oldrefs_to_nursery(struct list_s *lst)
{
    while (!list_is_empty(lst)) {
        object_t *obj = (object_t *)list_pop_item(lst);
        assert(!_is_in_nursery(obj));

        /* We must have GCFLAG_WRITE_BARRIER_CALLED so far.  If we
           don't, it's because the same object was stored in several
           segment's old_objects_pointing_to_young.  It's fine to
           ignore duplicates. */
        if ((obj->stm_flags & GCFLAG_WRITE_BARRIER_CALLED) == 0)
            continue;

        /* Remove the flag GCFLAG_WRITE_BARRIER_CALLED.  No live object
           should have this flag set after a nursery collection. */
        obj->stm_flags &= ~GCFLAG_WRITE_BARRIER_CALLED;

        /* Trace the 'obj' to replace pointers to nursery with pointers
           outside the nursery, possibly forcing nursery objects out
           and adding them to 'old_objects_pointing_to_young' as well. */
        trace_and_drag_out_of_nursery(obj);
    }
}

static void reset_nursery(void)
{
    /* reset the global amount-of-nursery-used-so-far */
    nursery_ctl.used = 0;

    /* reset the write locks */
    memset(write_locks + ((NURSERY_START >> 4) - READMARKER_START),
           0, NURSERY_SIZE >> 4);

    long i;
    for (i = 0; i < NB_SEGMENTS; i++) {
        struct stm_priv_segment_info_s *other_pseg = get_priv_segment(i);
        /* no race condition here, because all other threads are paused
           in safe points, so cannot be e.g. in _stm_allocate_slowpath() */
        uintptr_t old_end = other_pseg->real_nursery_section_end;
        other_pseg->real_nursery_section_end = 0;
        other_pseg->pub.v_nursery_section_end = 0;

        /* we don't need to actually reset the read markers, unless
           we run too many nursery collections in the same transaction:
           in the normal case it is enough to increase
           'transaction_read_version' without changing
           'min_read_version_outside_nursery'.
        */
        if (other_pseg->transaction_state == TS_NONE) {
            /* no transaction running now, nothing to do */
        }
        else if (other_pseg->pub.transaction_read_version < 0xff) {
            other_pseg->pub.transaction_read_version++;
            assert(0 < other_pseg->min_read_version_outside_nursery &&
                   other_pseg->min_read_version_outside_nursery
                     < other_pseg->pub.transaction_read_version);
        }
        else {
            /* however, when the value 0xff is reached, we are stuck
               and we need to clean all the nursery read markers.
               We'll be un-stuck when this transaction finishes. */
            char *read_markers = REAL_ADDRESS(other_pseg->pub.segment_base,
                                              NURSERY_START >> 4);
            memset(read_markers, 0, NURSERY_SIZE >> 4);
        }

        /* reset the creation markers */
        if (old_end > NURSERY_START) {
            char *creation_markers = REAL_ADDRESS(other_pseg->pub.segment_base,
                                                  NURSERY_START >> 8);
            assert(old_end < NURSERY_START + NURSERY_SIZE);
            memset(creation_markers, 0, (old_end - NURSERY_START) >> 8);
        }
        else {
            assert(old_end == 0 || old_end == NURSERY_START);
        }
    }
}

static void do_minor_collection(void)
{
    /* all other threads are paused in safe points during the whole
       minor collection */
    dprintf(("minor_collection\n"));
    assert(_has_mutex());
    assert(list_is_empty(old_objects_pointing_to_young));

    /* List of what we need to do and invariants we need to preserve
       -------------------------------------------------------------

       We must move out of the nursery any object found within the
       nursery.  This requires either one or NB_SEGMENTS copies,
       depending on the current write-state of the object.

       We need to move the mark stored in the write_locks, read_markers
       and creation_markers arrays.  The creation_markers need some care
       because they work at a coarser granularity of 256 bytes, so
       objects with an "on" mark should not be moved too close to
       objects with an "off" mark and vice-versa.

       Then we must trace (= look inside) some objects outside the
       nursery, and fix any pointer found that goes to a nursery object.
       This tracing itself needs to be done either once or NB_SEGMENTS
       times, depending on whether the object is fully in shared pages
       or not.  We assume that 'stmcb_size_rounded_up' produce the same
       results on all copies (i.e. don't depend on modifiable
       information).
    */

    //check_gcpage_still_shared();

    collect_roots_in_nursery();

    long i;
    for (i = 0; i < NB_SEGMENTS; i++) {
        struct stm_priv_segment_info_s *other_pseg = get_priv_segment(i);
        collect_oldrefs_to_nursery(other_pseg->old_objects_pointing_to_young);
    }

    collect_oldrefs_to_nursery(old_objects_pointing_to_young);

    reset_nursery();

    pages_make_shared_again(FIRST_NURSERY_PAGE, NB_NURSERY_PAGES);
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
