#ifndef _STM_CORE_H_
# error "must be compiled via stmgc.c"
#endif


/* Outside the nursery, we are taking from the highest addresses
   complete pages, one at a time, which uniformly contain objects
   of size "8 * N" for any "2 <= N < GC_N_SMALL_REQUESTS".  We are
   taking from the lowest addresses large objects, which are
   guaranteed to be at least 256 bytes long (actually 288),
   allocated by largemalloc.c.
*/

#define GC_N_SMALL_REQUESTS    36
#define GC_MEDIUM_REQUEST      (GC_N_SMALL_REQUESTS * 8)


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

static char *allocate_outside_nursery(uint64_t size)
{
    /* not thread-safe!  Use only when holding the mutex */
    assert(_has_mutex());

    OPT_ASSERT(size >= 16);
    OPT_ASSERT((size & 7) == 0);

    uint64_t index = size / 8;
    if (index < GC_N_SMALL_REQUESTS) {
        assert(index >= 2);
        // XXX! TEMPORARY!
        return allocate_outside_nursery(GC_MEDIUM_REQUEST);
    }
    else {
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

object_t *_stm_allocate_old(ssize_t size_rounded_up)
{
    /* XXX not thread-safe!  and only for tests, don't use when a
       transaction might be running! */
    assert(size_rounded_up >= 16);
    assert((size_rounded_up & 7) == 0);

    char *addr = large_malloc(size_rounded_up);

    if (addr + size_rounded_up > uninitialized_page_start) {
        uintptr_t pagenum =
            (uninitialized_page_start - stm_object_pages) / 4096UL;
        uintptr_t pagecount =
            (addr + size_rounded_up - uninitialized_page_start) / 4096UL + 20;
        uintptr_t pagemax =
            (uninitialized_page_stop - uninitialized_page_start) / 4096UL;
        if (pagecount > pagemax)
            pagecount = pagemax;
        pages_initialize_shared(pagenum, pagecount);

        uninitialized_page_start += pagecount * 4096UL;
    }

    memset(addr, 0, size_rounded_up);

    stm_char* o = (stm_char *)(addr - stm_object_pages);
    return (object_t *)o;
}
