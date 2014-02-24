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

    long i;
    for (i = 0; i < NB_SEGMENTS; i++)
        get_segment(i)->nursery_current = (stm_char *)NURSERY_START;
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
    assert((uintptr_t)obj < NB_PAGES * 4096UL);
    if (!_is_in_nursery(obj))
        return;

    /* If the object was already seen here, its first word was set
       to GCWORD_MOVED.  In that case, the forwarding location, i.e.
       where the object moved to, is stored in the second word in 'obj'. */
    object_t *TLPREFIX *pforwarded_array = (object_t *TLPREFIX *)obj;

    if (pforwarded_array[0] == GCWORD_MOVED) {
        *pobj = pforwarded_array[1];    /* already moved */
        return;
    }

    /* We need to make a copy of this object.  It goes either in
       a largemalloc.c-managed area, or if it's small enough, in
       one of the small uniform pages from gcpage.c.
    */
    char *realobj = REAL_ADDRESS(STM_SEGMENT->segment_base, obj);
    size_t size = stmcb_size_rounded_up((struct object_s *)realobj);
    object_t *nobj;

    if (1 /*size >= GC_MEDIUM_REQUEST*/) {

        /* case 1: object is not small enough.
           Ask gcpage.c for an allocation via largemalloc. */
        nobj = allocate_outside_nursery_large(size);

        /* Copy the object  */
        char *realnobj = REAL_ADDRESS(STM_SEGMENT->segment_base, nobj);
        memcpy(realnobj, realobj, size);
    }
    else {
        /* case "small enough" */
        abort();  //...
    }

    /* Done copying the object. */
    //dprintf(("%p -> %p\n", obj, nobj));
    pforwarded_array[0] = GCWORD_MOVED;
    pforwarded_array[1] = nobj;
    *pobj = nobj;

    /* Must trace the object later */
    LIST_APPEND(STM_PSEGMENT->old_objects_pointing_to_nursery, nobj);
}

static void collect_roots_in_nursery(void)
{
    stm_thread_local_t *tl = STM_SEGMENT->running_thread;
    object_t **current = tl->shadowstack;
    object_t **base = tl->shadowstack_base;
    while (current-- != base) {
        assert(*current != (object_t *)-1);
        minor_trace_if_young(current);
    }
}

static void collect_oldrefs_to_nursery(void)
{
    struct list_s *lst = STM_PSEGMENT->old_objects_pointing_to_nursery;

    while (!list_is_empty(lst)) {
        object_t *obj = (object_t *)list_pop_item(lst);
        assert(!_is_in_nursery(obj));

        /* We must not have GCFLAG_WRITE_BARRIER so far.  Add it now. */
        assert(!(obj->stm_flags & GCFLAG_WRITE_BARRIER));
        obj->stm_flags |= GCFLAG_WRITE_BARRIER;

        /* Trace the 'obj' to replace pointers to nursery with pointers
           outside the nursery, possibly forcing nursery objects out
           and adding them to 'old_objects_pointing_to_nursery' as well. */
        char *realobj = REAL_ADDRESS(STM_SEGMENT->segment_base, obj);
        stmcb_trace((struct object_s *)realobj, &minor_trace_if_young);
    }
}

static void reset_nursery(void)
{
    /* reset the nursery by zeroing it */
    size_t size;
    char *realnursery;

    realnursery = REAL_ADDRESS(STM_SEGMENT->segment_base, _stm_nursery_start);
    size = STM_SEGMENT->nursery_current - (stm_char *)_stm_nursery_start;
    memset(realnursery, 0, size);

    STM_SEGMENT->nursery_current = (stm_char *)_stm_nursery_start;
}

static void minor_collection(bool commit)
{
    assert(!_has_mutex());
    abort_if_needed();

    /* We must move out of the nursery any object found within the
       nursery.  All objects touched are either from the current
       transaction, or are from 'old_objects_pointing_to_young'.
       In all cases, we should only read and change objects belonging
       to the current segment.

       XXX improve: it might be possible to run this function in
       a safe-point but without the mutex, if we are careful
    */

    dprintf(("minor_collection commit=%d\n", (int)commit));

    if (STM_PSEGMENT->old_objects_pointing_to_nursery == NULL)
        STM_PSEGMENT->old_objects_pointing_to_nursery = list_create();

    collect_roots_in_nursery();

    collect_oldrefs_to_nursery();

    reset_nursery();

    assert(list_is_empty(STM_PSEGMENT->old_objects_pointing_to_nursery));
    if (!commit && STM_PSEGMENT->overflow_objects_pointing_to_nursery == NULL)
        STM_PSEGMENT->overflow_objects_pointing_to_nursery = list_create();
}

void stm_collect(long level)
{
    assert(level == 0);
    minor_collection(/*commit=*/ false);
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

    minor_collection(/*commit=*/ false);
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
