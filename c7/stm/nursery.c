#ifndef _STM_CORE_H_
# error "must be compiled via stmgc.c"
#endif

/************************************************************/

/* xxx later: divide the nursery into sections, and zero them
   incrementally.  For now we avoid the mess of maintaining a
   description of which parts of the nursery are already zeroed
   and which ones are not (caused by the fact that each
   transaction fills up a different amount).
*/

#define NURSERY_START         (FIRST_NURSERY_PAGE * 4096UL)
#define NURSERY_SIZE          (NB_NURSERY_PAGES * 4096UL)
#define NURSERY_END           (NURSERY_START + NURSERY_SIZE)

static uintptr_t _stm_nursery_start;
uintptr_t _stm_nursery_end;


/************************************************************/

static void setup_nursery(void)
{
    assert(_STM_FAST_ALLOC <= NURSERY_SIZE);
    _stm_nursery_start = NURSERY_START;
    _stm_nursery_end   = NURSERY_END;
}

static void teardown_nursery(void)
{
}

static inline bool _is_in_nursery(object_t *obj)
{
    assert((uintptr_t)obj >= NURSERY_START);
    return (uintptr_t)obj < NURSERY_END;
}

bool _stm_in_nursery(object_t *obj)
{
    return _is_in_nursery(obj);
}

#if 0
static bool _is_young(object_t *obj)
{
    return _is_in_nursery(obj);    /* for now */
}
#endif


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

#if 0
static void minor_trace_if_young(object_t **pobj)
{
    abort(); //...
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
    uintptr_t lock_idx = (((uintptr_t)obj) >> 4) - WRITELOCK_START;
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
        ((struct object_s *)copyobj)->stm_flags |= GCFLAG_WRITE_BARRIER_CALLED;

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

            lock_idx = (dataofs >> 4) - WRITELOCK_START;
            assert(write_locks[lock_idx] == 0);
            write_locks[lock_idx] = write_lock;

            /* Then, for each segment > 0, we need to repeat the
               memcpy() done above.  XXX This could be optimized if
               NB_SEGMENTS > 2 by reading all non-written copies from the
               same segment, instead of reading really all segments. */
            for (i = 1; i < NB_SEGMENTS; i++) {
                uintptr_t diff = get_segment_base(i) - stm_object_pages;
                memcpy(copyobj + diff, realobj + diff, size);
                ((struct object_s *)(copyobj + diff))->stm_flags |=
                    GCFLAG_WRITE_BARRIER_CALLED;
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
    //dprintf(("%p -> %p\n", obj, nobj));
    pforwarded_array[0] = GCWORD_MOVED;
    pforwarded_array[1] = nobj;
    *pobj = nobj;

    /* Must trace the object later */
    LIST_APPEND(old_objects_pointing_to_young, nobj);
}

static void collect_roots_in_nursery(void)
{
    stm_thread_local_t *tl = stm_all_thread_locals;
    do {
        object_t **current = tl->shadowstack;
        object_t **base = tl->shadowstack_base;
        while (current-- != base) {
            minor_trace_if_young(current);
        }
        tl = tl->next;
    } while (tl != stm_all_thread_locals);
}

static void trace_and_drag_out_of_nursery(object_t *obj)
{
    long i;
    for (i = 0; i < NB_SEGMENTS; i++) {
        struct object_s *realobj =
            (struct object_s *)REAL_ADDRESS(get_segment_base(i), obj);

        realobj->stm_flags |= GCFLAG_WRITE_BARRIER;

        stmcb_trace((struct object_s *)realobj, &minor_trace_if_young);

        if (i == 0 && is_in_shared_pages(obj)) {
            /* the object needs fixing only in one copy, because all copies
               are shared and identical. */
            break;
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
        abort();//...
        //if ((obj->stm_flags & GCFLAG_WRITE_BARRIER_CALLED) == 0)
        //    continue;

        /* The flag GCFLAG_WRITE_BARRIER_CALLED is going to be removed:
           no live object should have this flag set after a nursery
           collection.  It is done in either one or NB_SEGMENTS copies. */

        /* Trace the 'obj' to replace pointers to nursery with pointers
           outside the nursery, possibly forcing nursery objects out
           and adding them to 'old_objects_pointing_to_young' as well. */
        trace_and_drag_out_of_nursery(obj);
    }
}

static void reset_nursery(void)
{
    abort();//...
    /* reset the global amount-of-nursery-used-so-far */
    nursery_ctl.used = nursery_ctl.initial_value_of_used;

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
            abort();//...
            /*assert(0 < other_pseg->min_read_version_outside_nursery &&
                   other_pseg->min_read_version_outside_nursery
                   < other_pseg->pub.transaction_read_version);*/
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
            assert(old_end <= NURSERY_END);
            memset(creation_markers, 0, (old_end - NURSERY_START) >> 8);
        }
        else {
            assert(old_end == 0 || old_end == NURSERY_START);
        }
    }
}
#endif

static void minor_collection(void)
{
    assert(!_has_mutex());
    abort_if_needed();

    dprintf(("minor_collection\n"));

    abort();//...
#if 0

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

    collect_roots_in_nursery();

    long i;
    for (i = 0; i < NB_SEGMENTS; i++) {
        struct stm_priv_segment_info_s *other_pseg = get_priv_segment(i);
        collect_oldrefs_to_nursery(other_pseg->old_objects_pointing_to_young);
    }

    collect_oldrefs_to_nursery(old_objects_pointing_to_young);

    reset_nursery();

    pages_make_shared_again(FIRST_NURSERY_PAGE, NB_NURSERY_PAGES);
#endif
}


void stm_collect(long level)
{
    assert(level == 0);
    minor_collection();
}


/************************************************************/


object_t *_stm_allocate_slowpath(ssize_t size_rounded_up)
{
    /* may collect! */
    STM_SEGMENT->nursery_current -= size_rounded_up;  /* restore correct val */

 restart:
    stm_safe_point();

    OPT_ASSERT(size_rounded_up >= 16);
    OPT_ASSERT((size_rounded_up & 7) == 0);
    OPT_ASSERT(size_rounded_up < _STM_FAST_ALLOC);

    stm_char *p = STM_SEGMENT->nursery_current;
    stm_char *end = p + size_rounded_up;
    if ((uintptr_t)end <= NURSERY_END) {
        STM_SEGMENT->nursery_current = end;
        return (object_t *)p;
    }

    minor_collection();
    goto restart;
}

object_t *_stm_allocate_external(ssize_t size_rounded_up)
{
    abort();//...
}

#ifdef STM_TESTS
void _stm_set_nursery_free_count(uint64_t free_count)
{
    assert(free_count <= NURSERY_SIZE);
    _stm_nursery_start = NURSERY_END - free_count;

    long i;
    for (i = 0; i < NB_SEGMENTS; i++) {
        if ((uintptr_t)get_segment(i)->nursery_current < _stm_nursery_start)
            get_segment(i)->nursery_current = (stm_char *)_stm_nursery_start;
    }
}
#endif

static void check_nursery_at_transaction_start(void)
{
    assert((uintptr_t)STM_SEGMENT->nursery_current == _stm_nursery_start);
    uintptr_t i;
    for (i = 0; i < _stm_nursery_end - _stm_nursery_start; i++)
        assert(STM_SEGMENT->nursery_current[i] == 0);
}
