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

object_t *_stm_allocate_old(ssize_t size_rounded_up)
{
    /* only for tests */
    char *p = allocate_outside_nursery_large(size_rounded_up);
    memset(p, 0, size_rounded_up);

    object_t *o = (object_t *)(p - stm_object_pages);
    o->stm_flags = STM_FLAGS_PREBUILT;
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


static inline struct object_s *mark_first_seg(object_t *obj)
{
    return (struct object_s *)REAL_ADDRESS(stm_object_pages, obj);
}

static inline bool mark_is_visited(object_t *obj)
{
    uintptr_t lock_idx = (((uintptr_t)obj) >> 4) - WRITELOCK_START;
    assert(lock_idx >= 0);
    assert(lock_idx < sizeof(write_locks));
    return write_locks[lock_idx] != 0;
}

static inline void mark_set_visited(object_t *obj)
{
    uintptr_t lock_idx = (((uintptr_t)obj) >> 4) - WRITELOCK_START;
    write_locks[lock_idx] = 0xff;
}

static inline void mark_record_trace(object_t **pobj)
{
    /* takes a normal pointer to a thread-local pointer to an object */
    object_t *obj = *pobj;

    if (obj == NULL)
        return;
    if (mark_is_visited(obj))
        return;    /* already visited this object */

    mark_set_visited(obj);
    LIST_APPEND(mark_objects_to_trace, obj);
}

static void mark_collect_modified_objects(void)
{
    //...
}

static void mark_collect_roots(void)
{
    stm_thread_local_t *tl = stm_all_thread_locals;
    do {
        object_t **current = tl->shadowstack;
        object_t **base = tl->shadowstack_base;
        while (current-- != base) {
            assert(*current != (object_t *)-1);
            mark_record_trace(current);
        }
        mark_record_trace(&tl->thread_local_obj);

        tl = tl->next;
    } while (tl != stm_all_thread_locals);
}

static void mark_visit_all_objects(void)
{
    while (!list_is_empty(mark_objects_to_trace)) {
        object_t *obj = (object_t *)list_pop_item(mark_objects_to_trace);

        stmcb_trace(mark_first_seg(obj), &mark_record_trace);

        if (!is_fully_in_shared_pages(obj)) {
            abort();//xxx;
        }
    }
}

static inline bool largemalloc_keep_object_at(char *data)
{
    /* this is called by largemalloc_sweep() */
    return mark_is_visited((object_t *)(data - stm_object_pages));
}

static void sweep_large_objects(void)
{
    largemalloc_sweep();
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

    /* marking */
    mark_objects_to_trace = list_create();
    mark_collect_modified_objects();
    mark_collect_roots();
    mark_visit_all_objects();
    list_free(mark_objects_to_trace);
    mark_objects_to_trace = NULL;

    /* sweeping */
    mutex_pages_lock();
    sweep_large_objects();
    //sweep_uniform_pages();
    mutex_pages_unlock();

    dprintf((" | used after collection:  %ld\n",
             (long)pages_ctl.total_allocated));
    dprintf((" `----------------------------------------------\n"));

    reset_major_collection_requested();
}
