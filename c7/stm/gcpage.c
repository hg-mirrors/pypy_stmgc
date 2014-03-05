#ifndef _STM_CORE_H_
# error "must be compiled via stmgc.c"
#endif


static struct list_s *testing_prebuilt_objs = NULL;


static void setup_gcpage(void)
{
    char *base = stm_object_pages + END_NURSERY_PAGE * 4096UL;
    uintptr_t length = (NB_PAGES - END_NURSERY_PAGE) * 4096UL;
    _stm_largemalloc_init_arena(base, length);

    uninitialized_page_start = stm_object_pages + END_NURSERY_PAGE * 4096UL;
    uninitialized_page_stop  = stm_object_pages + NB_PAGES * 4096UL;
}

static void teardown_gcpage(void)
{
    memset(small_alloc, 0, sizeof(small_alloc));
    free_uniform_pages = NULL;
    LIST_FREE(testing_prebuilt_objs);
}


#define GCPAGE_NUM_PAGES   20

static void setup_N_pages(char *pages_addr, uint64_t num)
{
    pages_initialize_shared((pages_addr - stm_object_pages) / 4096UL, num);
}

static void grab_more_free_pages_for_small_allocations(void)
{
    /* grab N (= GCPAGE_NUM_PAGES) pages out of the top addresses */
    uintptr_t decrease_by = GCPAGE_NUM_PAGES * 4096;
    if (uninitialized_page_stop - uninitialized_page_start <= decrease_by)
        goto out_of_memory;

    uninitialized_page_stop -= decrease_by;

    if (!_stm_largemalloc_resize_arena(uninitialized_page_stop -
                                       uninitialized_page_start))
        goto out_of_memory;

    setup_N_pages(uninitialized_page_start, GCPAGE_NUM_PAGES);

    char *p = uninitialized_page_start;
    long i;
    for (i = 0; i < 16; i++) {
        *(char **)p = free_uniform_pages;
        free_uniform_pages = p;
    }
    return;

 out_of_memory:
    stm_fatalerror("out of memory!\n");   /* XXX */
}

static char *_allocate_small_slowpath(uint64_t size)
{
    /* not thread-safe!  Use only when holding the mutex */
    assert(_has_mutex());

    if (free_uniform_pages == NULL)
        grab_more_free_pages_for_small_allocations();

    abort();//...
}


static char *allocate_outside_nursery_large(uint64_t size)
{
    /* thread-safe: use the lock of pages.c to prevent any remapping
       from occurring under our feet */
    mutex_pages_lock();
    increment_total_allocated(size + LARGE_MALLOC_OVERHEAD);

    /* Allocate the object with largemalloc.c from the lower addresses. */
    char *addr = _stm_large_malloc(size);
    if (addr == NULL)
        stm_fatalerror("not enough memory!\n");

    if (addr + size > uninitialized_page_start) {
        uintptr_t npages;
        npages = (addr + size - uninitialized_page_start) / 4096UL;
        npages += GCPAGE_NUM_PAGES;
        if (uninitialized_page_stop - uninitialized_page_start <
                npages * 4096UL) {
            stm_fatalerror("out of memory!\n");   /* XXX */
        }
        setup_N_pages(uninitialized_page_start, npages);
        uninitialized_page_start += npages * 4096UL;
    }

    mutex_pages_unlock();

    return addr;
}

object_t *_stm_allocate_old(ssize_t size_rounded_up)
{
    /* only for tests xxx but stm_setup_prebuilt() uses this now too */
    char *p = allocate_outside_nursery_large(size_rounded_up);
    memset(p, 0, size_rounded_up);

    object_t *o = (object_t *)(p - stm_object_pages);
    o->stm_flags = GCFLAG_WRITE_BARRIER;

    if (testing_prebuilt_objs == NULL)
        testing_prebuilt_objs = list_create();
    LIST_APPEND(testing_prebuilt_objs, o);

    return o;
}


/************************************************************/


static void major_collection_if_requested(void)
{
    assert(!_has_mutex());
    if (!is_major_collection_requested())
        return;

    s_mutex_lock();

    if (is_major_collection_requested()) {   /* if still true */

        synchronize_all_threads();

        if (is_major_collection_requested()) {   /* if *still* true */
            major_collection_now_at_safe_point();
        }
    }

    s_mutex_unlock();
}


/************************************************************/


static struct list_s *mark_objects_to_trace;

#define WL_VISITED   255


static inline uintptr_t mark_loc(object_t *obj)
{
    uintptr_t lock_idx = (((uintptr_t)obj) >> 4) - WRITELOCK_START;
    assert(lock_idx >= 0);
    assert(lock_idx < sizeof(write_locks));
    return lock_idx;
}

static inline bool mark_visited_test(object_t *obj)
{
    uintptr_t lock_idx = mark_loc(obj);
    return write_locks[lock_idx] == WL_VISITED;
}

static inline bool mark_visited_test_and_set(object_t *obj)
{
    uintptr_t lock_idx = mark_loc(obj);
    if (write_locks[lock_idx] == WL_VISITED) {
        return true;
    }
    else {
        write_locks[lock_idx] = WL_VISITED;
        return false;
    }
}

static inline bool mark_visited_test_and_clear(object_t *obj)
{
    uintptr_t lock_idx = mark_loc(obj);
    if (write_locks[lock_idx] == WL_VISITED) {
        write_locks[lock_idx] = 0;
        return true;
    }
    else {
        return false;
    }
}

/************************************************************/


static inline void mark_single_flag_private(uintptr_t pagenum)
{
    if (flag_page_private[pagenum] == PRIVATE_PAGE) {
        assert(pagenum >= END_NURSERY_PAGE);
        assert(pagenum < NB_PAGES);
        flag_page_private[pagenum] = SEGMENT1_PAGE;
    }
    else {
        assert(flag_page_private[pagenum] == SHARED_PAGE ||
               flag_page_private[pagenum] == SEGMENT1_PAGE);
    }
}

static inline void mark_flag_page_private(object_t *obj, char *segment_base)
{
    uintptr_t first_page = ((uintptr_t)obj) / 4096UL;

    if (LIKELY((obj->stm_flags & GCFLAG_SMALL_UNIFORM) != 0)) {
        mark_single_flag_private(first_page);
    }
    else {
        char *realobj;
        size_t obj_size;
        uintptr_t end_page;

        /* get the size of the object */
        realobj = REAL_ADDRESS(segment_base, obj);
        obj_size = stmcb_size_rounded_up((struct object_s *)realobj);

        /* that's the page *following* the last page with the object */
        end_page = (((uintptr_t)obj) + obj_size + 4095) / 4096UL;

        while (first_page < end_page)
            mark_single_flag_private(first_page++);
    }
}

static void major_reshare_pages_range(uintptr_t first_page, uintptr_t end_page)
{
    uintptr_t i;
    for (i = first_page; i < end_page; i++) {

        switch (flag_page_private[i]) {

        case SEGMENT1_PAGE:
            /* this page stays private after major collection */
            flag_page_private[i] = PRIVATE_PAGE;
            break;

        case PRIVATE_PAGE:;
            /* this page becomes shared again.  No object in it was
               traced belonging to a segment other than 0.

               XXX This is maybe a too-strict condition, but the more
               general condition "all traced objects belong to the same
               segment" has problems with large objects in segments > 0.
               More precisely: we'd need to keep in the shared page the
               content of the objects (from segment > 0), but also the
               largemalloc's chunk data (stored in segment 0).
            */
#if NB_SEGMENTS != 2
#  error "limited to NB_SEGMENTS == 2"
#endif
            char *ppage0 = get_segment_base(0) + i * 4096;
            char *ppage1 = get_segment_base(1) + i * 4096;

            /* two cases for mapping pages to file-pages (fpages):
                - (0->0, 1->1)
                - (0->1, 1->0)
               Distinguish which case it is by hacking a lot */

            // 0->0,1->1 or 0->1,1->0
            /* map page 1 to fpage 0: */
            d_remap_file_pages(ppage1, 4096, i);
            // 0->0,1->0 or 0->1,1->0

            char oldvalue0 = *ppage0;
            char oldvalue1 = *ppage1;
            asm("":::"memory");
            *ppage0 = 1 + oldvalue1;
            asm("":::"memory");
            char newvalue1 = *ppage1;
            asm("":::"memory");
            *ppage0 = oldvalue0;
            /* if we are in 0->0,1->0, old and new are different:
               In this case we are done. We keep the largemalloc
               data structure and objects of ppage0/fpage0 */
            if (oldvalue1 == newvalue1) {
                // 0->1,1->0
                /* ppage0/fpage1 has the data structure that we want
                   in ppage1/fpage0, so we copy it */
                pagecopy(ppage1, ppage0);   // copy from page0 to page1,
                //         i.e. from the underlying memory seg1 to seg0
                d_remap_file_pages(ppage0, 4096, i);
                // 0->0,1->0
            }
            flag_page_private[i] = SHARED_PAGE;

            increment_total_allocated(-4096 * (NB_SEGMENTS-1));
            break;

        case SHARED_PAGE:
            break;     /* stay shared */

        default:
            assert(!"unexpected flag_page_private");
        }
    }
}

static void major_reshare_pages(void)
{
    /* re-share pages if possible.  Each re-sharing decreases
       total_allocated by 4096. */
    major_reshare_pages_range(
        END_NURSERY_PAGE,       /* not the nursery! */
        (uninitialized_page_start - stm_object_pages) / 4096UL);
    major_reshare_pages_range(
        (uninitialized_page_stop - stm_object_pages) / 4096UL,
        NB_PAGES);
}

/************************************************************/


static inline void mark_record_trace(object_t **pobj)
{
    /* takes a normal pointer to a thread-local pointer to an object */
    object_t *obj = *pobj;

    if (obj == NULL || mark_visited_test_and_set(obj))
        return;    /* already visited this object */

    LIST_APPEND(mark_objects_to_trace, obj);

    /* Note: this obj might be visited already, but from a different
       segment.  We ignore this case and skip re-visiting the object
       anyway.  The idea is that such an object is old (not from the
       current transaction), otherwise it would not be possible to see
       it in two segments; and moreover it is not modified, otherwise
       mark_trace() would have been called on two different segments
       already.  That means that this object is identical in all
       segments and only needs visiting once.  (It may actually be in a
       shared page, or maybe not.)
    */
}

static void mark_trace(object_t *obj, char *segment_base)
{
    assert(list_is_empty(mark_objects_to_trace));

    while (1) {

        /* first, if we're not seeing segment 0, we must change the
           flags in flag_page_private[] from PRIVATE_PAGE to
           SEGMENT1_PAGE, which will mean "can't re-share" */
        if (segment_base != stm_object_pages && RESHARE_PAGES)
            mark_flag_page_private(obj, segment_base);

        /* trace into the object (the version from 'segment_base') */
        struct object_s *realobj =
            (struct object_s *)REAL_ADDRESS(segment_base, obj);
        stmcb_trace(realobj, &mark_record_trace);

        if (list_is_empty(mark_objects_to_trace))
            break;

        obj = (object_t *)list_pop_item(mark_objects_to_trace);
    }
}

static inline void mark_visit_object(object_t *obj, char *segment_base)
{
    if (obj == NULL || mark_visited_test_and_set(obj))
        return;
    mark_trace(obj, segment_base);
}

static void mark_visit_from_roots(void)
{

    if (testing_prebuilt_objs != NULL) {
        LIST_FOREACH_R(testing_prebuilt_objs, object_t * /*item*/,
                       mark_visit_object(item, get_segment_base(0)));
    }

    /* Do the following twice, so that we trace first the objects from
       segment 0, and then all others.  XXX This is a hack to make it
       more likely that we'll be able to re-share pages. */

    int must_be_zero;
    for (must_be_zero = 1; must_be_zero >= 0; must_be_zero--) {

        stm_thread_local_t *tl = stm_all_thread_locals;
        do {
            /* If 'tl' is currently running, its 'associated_segment_num'
               field is the segment number that contains the correct
               version of its overflowed objects.  If not, then the
               field is still some correct segment number, and it doesn't
               matter which one we pick. */
            char *segment_base = get_segment_base(tl->associated_segment_num);

            if (must_be_zero == (segment_base == get_segment_base(0))) {

                object_t **current = tl->shadowstack;
                object_t **base = tl->shadowstack_base;
                while (current-- != base) {
                    assert(*current != (object_t *)-1);
                    mark_visit_object(*current, segment_base);
                }
                mark_visit_object(tl->thread_local_obj, segment_base);
            }

            tl = tl->next;
        } while (tl != stm_all_thread_locals);
    }

    long i;
    for (i = 0; i < NB_SEGMENTS; i++) {
        if (get_priv_segment(i)->transaction_state != TS_NONE)
            mark_visit_object(
                get_priv_segment(i)->threadlocal_at_start_of_transaction,
                get_segment_base(i));
    }
}

static void mark_visit_from_modified_objects(void)
{
    /* The modified objects are the ones that may exist in two different
       versions: one in the segment that modified it, and another in
       all other segments. */
    long i;
    for (i = 0; i < NB_SEGMENTS; i++) {
        char *base1 = get_segment_base(i);   /* two different segments */
        char *base2 = get_segment_base(!i);

        LIST_FOREACH_R(
            get_priv_segment(i)->modified_old_objects,
            object_t * /*item*/,
            ({
                mark_visited_test_and_set(item);
                mark_trace(item, base1);
                mark_trace(item, base2);
            }));
    }
}

static void clean_up_segment_lists(void)
{
    long i;
    for (i = 0; i < NB_SEGMENTS; i++) {
        struct stm_priv_segment_info_s *pseg = get_priv_segment(i);
        struct list_s *lst;

        /* 'objects_pointing_to_nursery' should be empty, but isn't
           necessarily because it also lists objects that have been
           written to but don't actually point to the nursery.  Clear
           it up and set GCFLAG_WRITE_BARRIER again on the objects. */
        lst = pseg->objects_pointing_to_nursery;
        if (lst != NULL) {
            LIST_FOREACH_R(lst, uintptr_t /*item*/,
                ({
                    struct object_s *realobj = (struct object_s *)
                        REAL_ADDRESS(pseg->pub.segment_base, item);
                    assert(!(realobj->stm_flags & GCFLAG_WRITE_BARRIER));
                    realobj->stm_flags |= GCFLAG_WRITE_BARRIER;
                }));
            list_clear(lst);
        }

        /* Remove from 'large_overflow_objects' all objects that die */
        lst = pseg->large_overflow_objects;
        if (lst != NULL) {
            uintptr_t n = list_count(lst);
            while (n > 0) {
                object_t *obj = (object_t *)list_item(lst, --n);
                if (!mark_visited_test(obj)) {
                    list_set_item(lst, n, list_pop_item(lst));
                }
            }
        }
    }
}

static inline bool largemalloc_keep_object_at(char *data)
{
    /* this is called by _stm_largemalloc_sweep() */
    return mark_visited_test_and_clear((object_t *)(data - stm_object_pages));
}

static void sweep_large_objects(void)
{
    _stm_largemalloc_sweep();
}

static void clean_write_locks(void)
{
    /* the write_locks array, containing the visit marker during
       major collection, is cleared in sweep_large_objects() for
       large objects, but is not cleared for small objects.
       Clear it now. */
    object_t *loc2 = (object_t *)(uninitialized_page_stop - stm_object_pages);
    uintptr_t lock2_idx = mark_loc(loc2 - 1) + 1;

    assert_memset_zero(write_locks, lock2_idx);
    memset(write_locks + lock2_idx, 0, sizeof(write_locks) - lock2_idx);
}

static void major_set_write_locks(void)
{
    /* restore the write locks on the modified objects */
    long i;
    for (i = 0; i < NB_SEGMENTS; i++) {
        struct stm_priv_segment_info_s *pseg = get_priv_segment(i);

        LIST_FOREACH_R(
            pseg->modified_old_objects,
            object_t * /*item*/,
            ({
                uintptr_t lock_idx = mark_loc(item);
                assert(write_locks[lock_idx] == 0);
                write_locks[lock_idx] = pseg->write_lock_num;
            }));
    }
}

static void major_collection_now_at_safe_point(void)
{
    dprintf(("\n"));
    dprintf((" .----- major collection -----------------------\n"));
    assert(_has_mutex());

    /* first, force a minor collection in each of the other segments */
    major_do_minor_collections();

    dprintf((" | used before collection: %ld\n",
             (long)pages_ctl.total_allocated));

    /* marking */
    LIST_CREATE(mark_objects_to_trace);
    mark_visit_from_modified_objects();
    mark_visit_from_roots();
    LIST_FREE(mark_objects_to_trace);

    /* cleanup */
    clean_up_segment_lists();

    /* sweeping */
    mutex_pages_lock();
    if (RESHARE_PAGES)
        major_reshare_pages();
    sweep_large_objects();
    //sweep_uniform_pages();
    mutex_pages_unlock();

    clean_write_locks();
    major_set_write_locks();

    dprintf((" | used after collection:  %ld\n",
             (long)pages_ctl.total_allocated));
    dprintf((" `----------------------------------------------\n"));

    reset_major_collection_requested();
}
