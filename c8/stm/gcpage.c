#ifndef _STM_CORE_H_
# error "must be compiled via stmgc.c"
#endif

static struct list_s *testing_prebuilt_objs = NULL;
static struct tree_s *tree_prebuilt_objs = NULL;     /* XXX refactor */


static void setup_gcpage(void)
{
    char *base = stm_object_pages + END_NURSERY_PAGE * 4096UL;
    uintptr_t length = (NB_PAGES - END_NURSERY_PAGE) * 4096UL;
    _stm_largemalloc_init_arena(base, length);

    uninitialized_page_start = stm_object_pages + END_NURSERY_PAGE * 4096UL;
    uninitialized_page_stop  = uninitialized_page_start + NB_SHARED_PAGES * 4096UL;
}

static void teardown_gcpage(void)
{
    LIST_FREE(testing_prebuilt_objs);
    if (tree_prebuilt_objs != NULL) {
        tree_free(tree_prebuilt_objs);
        tree_prebuilt_objs = NULL;
    }
}



static void setup_N_pages(char *pages_addr, long num)
{
    /* make pages accessible in sharing segment only (pages already
       PROT_READ/WRITE (see setup.c), but not marked accessible as page
       status). */

    /* lock acquiring maybe not necessary because the affected pages don't
       need privatization protection. (but there is an assert right
       now to enforce that XXXXXX) */
    acquire_all_privatization_locks();

    uintptr_t p = (pages_addr - stm_object_pages) / 4096UL;
    dprintf(("setup_N_pages(%p, %lu): pagenum %lu\n", pages_addr, num, p));
    while (num-->0) {
        /* XXX: page_range_mark_accessible() */
        page_mark_accessible(0, p + num);
    }

    release_all_privatization_locks();
}


static int lock_growth_large = 0;

static stm_char *allocate_outside_nursery_large(uint64_t size)
{
    /* Allocate the object with largemalloc.c from the lower addresses. */
    char *addr = _stm_large_malloc(size);
    if (addr == NULL)
        stm_fatalerror("not enough memory!");

    if (LIKELY(addr + size <= uninitialized_page_start))
        return (stm_char*)(addr - stm_object_pages);


    /* uncommon case: need to initialize some more pages */
    spinlock_acquire(lock_growth_large);

    char *start = uninitialized_page_start;
    if (addr + size > start) {
        uintptr_t npages;
        npages = (addr + size - start) / 4096UL;
        npages += GCPAGE_NUM_PAGES;
        if (uninitialized_page_stop - start < npages * 4096UL) {
            stm_fatalerror("out of memory!");   /* XXX */
        }
        setup_N_pages(start, npages);
        if (!__sync_bool_compare_and_swap(&uninitialized_page_start,
                                          start,
                                          start + npages * 4096UL)) {
            stm_fatalerror("uninitialized_page_start changed?");
        }
    }

    dprintf(("allocate_outside_nursery_large(%lu): %p, page=%lu\n",
             size, (char*)(addr - stm_object_pages),
             (uintptr_t)(addr - stm_object_pages) / 4096UL));

    spinlock_release(lock_growth_large);
    return (stm_char*)(addr - stm_object_pages);
}

object_t *_stm_allocate_old(ssize_t size_rounded_up)
{
    /* only for tests xxx but stm_setup_prebuilt() uses this now too */
    stm_char *p = allocate_outside_nursery_large(size_rounded_up);
    object_t *o = (object_t *)p;

    memset(get_virtual_address(STM_SEGMENT->segment_num, o), 0, size_rounded_up);
    o->stm_flags = GCFLAG_WRITE_BARRIER;

    dprintf(("allocate_old(%lu): %p, seg=%d, page=%lu\n",
             size_rounded_up, p,
             get_segment_of_linear_address(stm_object_pages + (uintptr_t)p),
             (uintptr_t)p / 4096UL));
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

        synchronize_all_threads(STOP_OTHERS_UNTIL_MUTEX_UNLOCK);

        if (is_major_collection_requested()) {   /* if *still* true */
            major_collection_now_at_safe_point();
        }

    }

    s_mutex_unlock();
}


/************************************************************/

/* objects to trace are traced in the sharing seg0 or in a
   certain segment if there exist modifications there.
   All other segments' versions should be identical to seg0's
   version and thus don't need tracing. */
static struct list_s *marked_objects_to_trace;

/* we use the sharing seg0's pages for the GCFLAG_VISITED flag */

static inline struct object_s *mark_loc(object_t *obj)
{
    /* uses the memory in seg0 for marking: */
    struct object_s *result = (struct object_s*)REAL_ADDRESS(stm_object_pages, obj);
    return result;
}

static inline bool mark_visited_test(object_t *obj)
{
    struct object_s *realobj = mark_loc(obj);
    return !!(realobj->stm_flags & GCFLAG_VISITED);
}

static inline bool mark_visited_test_and_set(object_t *obj)
{
    struct object_s *realobj = mark_loc(obj);
    if (realobj->stm_flags & GCFLAG_VISITED) {
        return true;
    }
    else {
        realobj->stm_flags |= GCFLAG_VISITED;
        return false;
    }
}

static inline bool mark_visited_test_and_clear(object_t *obj)
{
    struct object_s *realobj = mark_loc(obj);
    if (realobj->stm_flags & GCFLAG_VISITED) {
        realobj->stm_flags &= ~GCFLAG_VISITED;
        return true;
    }
    else {
        return false;
    }
}


/************************************************************/

static inline void mark_record_trace(object_t **pobj)
{
    /* takes a normal pointer to a thread-local pointer to an object */
    object_t *obj = *pobj;

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
    if (obj == NULL || mark_visited_test_and_set(obj))
        return;    /* already visited this object */

    LIST_APPEND(marked_objects_to_trace, obj);
}


static void mark_and_trace(object_t *obj, char *segment_base)
{
    /* mark the obj and trace all reachable objs from it */

    assert(list_is_empty(marked_objects_to_trace));

    while (1) {
        /* trace into the object (the version from 'segment_base') */
        struct object_s *realobj =
            (struct object_s *)REAL_ADDRESS(segment_base, obj);
        stmcb_trace(realobj, &mark_record_trace);

        if (list_is_empty(marked_objects_to_trace))
            break;

        obj = (object_t *)list_pop_item(marked_objects_to_trace);
    }
}

static inline void mark_visit_object(object_t *obj, char *segment_base)
{
    /* if already visited, don't trace */
    if (obj == NULL || mark_visited_test_and_set(obj))
        return;
    mark_and_trace(obj, segment_base);
}

static void *mark_visit_objects_from_ss(void *_, const void *slice, size_t size)
{
    const struct stm_shadowentry_s *p, *end;
    p = (const struct stm_shadowentry_s *)slice;
    end = (const struct stm_shadowentry_s *)(slice + size);
    for (; p < end; p++)
        if ((((uintptr_t)p->ss) & 3) == 0)
            mark_visit_object(p->ss, stm_object_pages); // seg0
    return NULL;
}

static void assert_obj_accessible_in(long segnum, object_t *obj)
{
#ifndef NDEBUG
    uintptr_t page = (uintptr_t)obj / 4096UL;
    assert(get_page_status_in(segnum, page) == PAGE_ACCESSIBLE);

    struct object_s *realobj =
        (struct object_s *)REAL_ADDRESS(get_segment_base(segnum), obj);

    size_t obj_size = stmcb_size_rounded_up(realobj);
    uintptr_t count = obj_size / 4096UL + 1;
    while (count--> 0) {
        assert(get_page_status_in(segnum, page) == PAGE_ACCESSIBLE);
        page++;
    }
#endif
}


static void mark_visit_from_modified_objects(void)
{
    /* look for modified objects in segments and mark all of them
       for further tracing (XXX: don't if we are going to share
       some of the pages) */

    long i;
    for (i = 1; i < NB_SEGMENTS; i++) {
        char *base = get_segment_base(i);

        LIST_FOREACH_R(
            get_priv_segment(i)->modified_old_objects,
            object_t * /*item*/,
            ({
                /* All modified objs have all pages accessible for now.
                   This is because we create a backup of the whole obj
                   and thus make all pages accessible. */
                assert_obj_accessible_in(i, item);

                mark_visited_test_and_set(item);
                mark_and_trace(item, stm_object_pages);  /* shared, committed version */
                mark_and_trace(item, base);          /* private, modified version */
            }));
    }
}

static void mark_visit_from_roots(void)
{
    if (testing_prebuilt_objs != NULL) {
        LIST_FOREACH_R(testing_prebuilt_objs, object_t * /*item*/,
                       mark_visit_object(item, stm_object_pages)); // seg0
    }

    stm_thread_local_t *tl = stm_all_thread_locals;
    do {
        /* look at all objs on the shadow stack (they are old but may
           be uncommitted so far, so only exist in the associated_segment_num).

           However, since we just executed a minor collection, they were
           all synced to the sharing seg0. Thus we can trace them there.

           If they were again modified since then, they were traced
           by mark_visit_from_modified_object() already.
        */
        struct stm_shadowentry_s *current = tl->shadowstack;
        struct stm_shadowentry_s *base = tl->shadowstack_base;
        while (current-- != base) {
            if ((((uintptr_t)current->ss) & 3) == 0)
                mark_visit_object(current->ss, stm_object_pages);
        }

        tl = tl->next;
    } while (tl != stm_all_thread_locals);

    /* also visit all objs in the rewind-shadowstack */
    long i;
    for (i = 1; i < NB_SEGMENTS; i++) {
        if (get_priv_segment(i)->transaction_state != TS_NONE) {
            stm_rewind_jmp_enum_shadowstack(
                get_segment(i)->running_thread,
                mark_visit_objects_from_ss);
        }
    }
}


static void clean_up_segment_lists(void)
{
#pragma push_macro("STM_PSEGMENT")
#pragma push_macro("STM_SEGMENT")
#undef STM_PSEGMENT
#undef STM_SEGMENT

    long i;
    for (i = 1; i < NB_SEGMENTS; i++) {
        struct stm_priv_segment_info_s *pseg = get_priv_segment(i);
        struct list_s *lst;

        /* 'objects_pointing_to_nursery' should be empty, but isn't
           necessarily because it also lists objects that have been
           written to but don't actually point to the nursery.  Clear
           it up and set GCFLAG_WRITE_BARRIER again on the objects.
           This is the case for transactions where
               MINOR_NOTHING_TO_DO() == true
           but they still did write-barriers on objects
        */
        lst = pseg->objects_pointing_to_nursery;
        if (!list_is_empty(lst)) {
            abort(); // check that there is a test
            LIST_FOREACH_R(lst, object_t* /*item*/,
                ({
                    struct object_s *realobj = (struct object_s *)
                        REAL_ADDRESS(pseg->pub.segment_base, (uintptr_t)item);

                    assert(realobj->stm_flags & GCFLAG_WB_EXECUTED);
                    assert(!(realobj->stm_flags & GCFLAG_WRITE_BARRIER));

                    realobj->stm_flags |= GCFLAG_WRITE_BARRIER;
                }));
            list_clear(lst);
        } else {
            /* if here MINOR_NOTHING_TO_DO() was true before, it's like
               we "didn't do a collection" at all. So nothing to do on
               modified_old_objs. */
        }
    }
#pragma pop_macro("STM_SEGMENT")
#pragma pop_macro("STM_PSEGMENT")
}

static inline bool largemalloc_keep_object_at(char *data)
{
    /* this is called by _stm_largemalloc_sweep() */
    object_t *obj = (object_t *)(data - stm_object_pages);
    dprintf(("keep obj %p ? -> %d\n", obj, mark_visited_test(obj)));
    if (!mark_visited_test_and_clear(obj)) {
        /* This is actually needed in order to avoid random write-read
           conflicts with objects read and freed long in the past.
           It is probably rare enough, but still, we want to avoid any
           false conflict. (test_random hits it sometimes) */
        long i;
        for (i = 1; i < NB_SEGMENTS; i++) {
            /* reset read marker */
            *((char *)(get_segment_base(i) + (((uintptr_t)obj) >> 4))) = 0;
        }
        return false;
    }
    return true;
}

static void sweep_large_objects(void)
{
    _stm_largemalloc_sweep();
}

static void major_collection_now_at_safe_point(void)
{
    dprintf(("\n"));
    dprintf((" .----- major collection -----------------------\n"));
    assert(_has_mutex());

    /* first, force a minor collection in each of the other segments */
    major_do_validation_and_minor_collections();

    dprintf((" | used before collection: %ld\n",
             (long)pages_ctl.total_allocated));

    /* only necessary because of assert that fails otherwise (XXX) */
    acquire_all_privatization_locks();

    DEBUG_EXPECT_SEGFAULT(false);

    /* marking */
    LIST_CREATE(marked_objects_to_trace);
    mark_visit_from_modified_objects();
    mark_visit_from_roots();
    LIST_FREE(marked_objects_to_trace);

    /* /\* cleanup *\/ */
    clean_up_segment_lists();

    /* /\* sweeping *\/ */
    sweep_large_objects();
    /* //sweep_uniform_pages(); */

    dprintf((" | used after collection:  %ld\n",
             (long)pages_ctl.total_allocated));
    dprintf((" `----------------------------------------------\n"));

    reset_major_collection_requested();

    DEBUG_EXPECT_SEGFAULT(true);

    release_all_privatization_locks();
}
