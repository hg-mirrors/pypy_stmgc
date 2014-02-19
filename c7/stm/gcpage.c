#ifndef _STM_CORE_H_
# error "must be compiled via stmgc.c"
#endif


static void setup_gcpage(void)
{
    /* NB. the very last page is not used, which allows a speed-up in
       reset_all_creation_markers() */
    char *base = stm_object_pages + END_NURSERY_PAGE * 4096UL;
    uintptr_t length = (NB_PAGES - END_NURSERY_PAGE - 1) * 4096UL;
    largemalloc_init_arena(base, length);

    uninitialized_page_start = stm_object_pages + END_NURSERY_PAGE * 4096UL;
    uninitialized_page_stop  = stm_object_pages + (NB_PAGES - 1) * 4096UL;

    assert(GC_MEDIUM_REQUEST >= (1 << 8));
}

static void teardown_gcpage(void)
{
    memset(small_alloc_shared, 0, sizeof(small_alloc_shared));
    memset(small_alloc_privtz, 0, sizeof(small_alloc_privtz));
    free_pages = NULL;
}

static void check_gcpage_still_shared(void)
{
    //...;
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

    if (!largemalloc_resize_arena(uninitialized_page_stop -
                                  uninitialized_page_start))
        goto out_of_memory;

    setup_N_pages(uninitialized_page_start, GCPAGE_NUM_PAGES);

    char *p = uninitialized_page_start;
    long i;
    for (i = 0; i < 16; i++) {
        *(char **)p = free_pages;
        free_pages = p;
    }
    return;

 out_of_memory:
    stm_fatalerror("out of memory!\n");
}

static char *_allocate_small_slowpath(
        struct small_alloc_s small_alloc[], uint64_t size)
{
    if (free_pages == NULL)
        grab_more_free_pages_for_small_allocations();

    abort();//...
}


#if 0
static char *allocate_outside_nursery(uint64_t size)
{
    /* not thread-safe!  Use only when holding the mutex */
    assert(_has_mutex());

    OPT_ASSERT(size >= 16);
    OPT_ASSERT((size & 7) == 0);

    uint64_t index = size / 8;
    assert(index >= GC_N_SMALL_REQUESTS);
    {
        /* The object is too large to fit inside the uniform pages.
           Allocate it with largemalloc.c from the lower addresses */
        char *addr = large_malloc(size);

        if (addr + size > uninitialized_page_start) {
            uintptr_t pagenum =
                (uninitialized_page_start - stm_object_pages) / 4096UL;
            uintptr_t pagecount =
                (addr + size - uninitialized_page_start) / 4096UL + 20;
            uintptr_t pagemax =
                (uninitialized_page_stop - uninitialized_page_start) / 4096UL;
            if (pagecount > pagemax)
                pagecount = pagemax;
            pages_initialize_shared(pagenum, pagecount);

            uninitialized_page_start += pagecount * 4096UL;
        }

        assert(get_single_creation_marker(
                   (stm_char *)(addr - stm_object_pages)) == 0);
        return addr;
    }
}
#endif

object_t *_stm_allocate_old(ssize_t size_rounded_up)
{
    /* XXX not thread-safe!  and only for tests, don't use when a
       transaction might be running! */
    assert(size_rounded_up >= 16);
    assert((size_rounded_up & 7) == 0);

    char *addr = large_malloc(size_rounded_up);

    if (addr + size_rounded_up > uninitialized_page_start) {
        uintptr_t npages;
        npages = (addr + size_rounded_up - uninitialized_page_start) / 4096UL;
        npages += GCPAGE_NUM_PAGES;
        setup_N_pages(uninitialized_page_start, npages);
        uninitialized_page_start += npages * 4096UL;
    }

    memset(addr, 0, size_rounded_up);

    stm_char* o = (stm_char *)(addr - stm_object_pages);
    return (object_t *)o;
}
