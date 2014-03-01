#ifndef _STM_CORE_H_
# error "must be compiled via stmgc.c"
#endif


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

static struct list_s *testing_prebuilt_objs = NULL;

object_t *_stm_allocate_old(ssize_t size_rounded_up)
{
    /* only for tests */
    char *p = allocate_outside_nursery_large(size_rounded_up);
    memset(p, 0, size_rounded_up);

    object_t *o = (object_t *)(p - stm_object_pages);
    o->stm_flags = STM_FLAGS_PREBUILT;

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

#define WL_VISITED   42


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
    assert(write_locks[lock_idx] == 0 || write_locks[lock_idx] == WL_VISITED);
    return write_locks[lock_idx] != 0;
}

static inline bool mark_visited_test_and_set(object_t *obj)
{
    uintptr_t lock_idx = mark_loc(obj);
    assert(write_locks[lock_idx] == 0 || write_locks[lock_idx] == WL_VISITED);
    if (write_locks[lock_idx] != 0) {
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
    assert(write_locks[lock_idx] == 0 || write_locks[lock_idx] == WL_VISITED);
    if (write_locks[lock_idx] != 0) {
        write_locks[lock_idx] = 0;
        return true;
    }
    else {
        return false;
    }
}

static inline void mark_record_trace(object_t **pobj)
{
    /* takes a normal pointer to a thread-local pointer to an object */
    object_t *obj = *pobj;

    if (obj == NULL || mark_visited_test_and_set(obj))
        return;    /* already visited this object */

    LIST_APPEND(mark_objects_to_trace, obj);
}

static void mark_from_object(object_t *obj, char *segment_base)
{
    if (obj == NULL || mark_visited_test_and_set(obj))
        return;

    assert(list_is_empty(mark_objects_to_trace));

    while (1) {
        struct object_s *realobj =
            (struct object_s *)REAL_ADDRESS(segment_base, obj);
        stmcb_trace(realobj, &mark_record_trace);

        if (list_is_empty(mark_objects_to_trace))
            break;

        obj = (object_t *)list_pop_item(mark_objects_to_trace);
    }
}

static void mark_visit_from_roots(void)
{
    stm_thread_local_t *tl = stm_all_thread_locals;
    do {
        /* If 'tl' is currently running, its 'associated_segment_num'
           field is the segment number that contains the correct
           version of its overflowed objects.  If not, then the
           field is still some correct segment number, and it doesn't
           matter which one we pick. */
        char *segment_base = get_segment_base(tl->associated_segment_num);

        object_t **current = tl->shadowstack;
        object_t **base = tl->shadowstack_base;
        while (current-- != base) {
            assert(*current != (object_t *)-1);
            mark_from_object(*current, segment_base);
        }
        mark_from_object(tl->thread_local_obj, segment_base);

        tl = tl->next;
    } while (tl != stm_all_thread_locals);

    if (testing_prebuilt_objs != NULL) {
        LIST_FOREACH_R(testing_prebuilt_objs, object_t * /*item*/,
                       mark_from_object(item, get_segment_base(0)));
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
                mark_from_object(item, base1);
                mark_from_object(item, base2);
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
    object_t *loc2 = (object_t *)(uninitialized_page_stop  - stm_object_pages);
    uintptr_t lock2_idx = mark_loc(loc2 - 1) + 1;

#ifdef STM_TESTS
    long _i;
    for (_i=0; _i<lock2_idx; _i++) {
        assert(write_locks[_i] == 0);
        if (_i == 1000000) break;  /* ok, stop testing */
    }
#endif
    memset(write_locks + lock2_idx, 0, sizeof(write_locks) - lock2_idx);
}

static void major_clear_write_locks(void)
{
    long i;
    for (i = 0; i < NB_SEGMENTS; i++) {
        struct stm_priv_segment_info_s *pseg = get_priv_segment(i);

        LIST_FOREACH_R(
            pseg->modified_old_objects,
            object_t * /*item*/,
            ({
                assert(item != NULL);

                uintptr_t lock_idx = mark_loc(item);
                assert(write_locks[lock_idx] == pseg->write_lock_num);
                write_locks[lock_idx] = 0;
            }));
    }
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
    dprintf((" .----- major_collection_now_at_safe_point -----\n"));
    assert(_has_mutex());

    /* first, force a minor collection in each of the other segments */
    major_do_minor_collections();

    dprintf((" | used before collection: %ld\n",
             (long)pages_ctl.total_allocated));

    major_clear_write_locks();

    /* marking */
    mark_objects_to_trace = list_create();
    mark_visit_from_modified_objects();
    mark_visit_from_roots();
    list_free(mark_objects_to_trace);
    mark_objects_to_trace = NULL;

    /* cleanup */
    clean_up_segment_lists();

    /* sweeping */
    mutex_pages_lock();
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
