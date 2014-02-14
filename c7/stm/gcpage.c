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

    uninitialized_page_start = (stm_char *)(END_NURSERY_PAGE * 4096UL);
    uninitialized_page_stop = (stm_char *)((NB_PAGES - 1) * 4096UL);
}

object_t *_stm_allocate_old(ssize_t size_rounded_up)
{
    /* XXX not thread-safe! */
    char *addr = large_malloc(size_rounded_up);
    stm_char* o = (stm_char *)(addr - stm_object_pages);

    if (o + size_rounded_up > uninitialized_page_start) {
        uintptr_t pagenum =
            ((uint64_t)uninitialized_page_start) / 4096UL;
        uintptr_t pagecount =
            (o + size_rounded_up - uninitialized_page_start) / 4096UL + 20;
        uintptr_t pagemax =
            (uninitialized_page_stop - uninitialized_page_start) / 4096UL;
        if (pagecount > pagemax)
            pagecount = pagemax;
        pages_initialize_shared(pagenum, pagecount);

        uninitialized_page_start += pagecount * 4096UL;
    }

    memset(addr, 0, size_rounded_up);

    if (CROSS_PAGE_BOUNDARY(o, o + size_rounded_up))
        ((object_t *)o)->stm_flags = GCFLAG_CROSS_PAGE;

    return (object_t *)o;
}
