#ifndef _STM_CORE_H_
# error "must be compiled via stmgc.c"
#endif


static void setup_gcpage(void)
{
    /* NB. the very last page is not used, which allows a speed-up in
       reset_all_creation_markers() */
    char *base = stm_object_pages + END_NURSERY_PAGE * 4096UL;
    uintptr_t length = (NB_PAGES - END_NURSERY_PAGE - 1) * 4096UL;
    _stm_largemalloc_init_arena(base, length);

    uninitialized_page_start = stm_object_pages + END_NURSERY_PAGE * 4096UL;
    uninitialized_page_stop  = stm_object_pages + NB_PAGES * 4096UL;

    assert(GC_MEDIUM_REQUEST >= (1 << 8));
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
