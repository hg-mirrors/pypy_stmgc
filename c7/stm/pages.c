#ifndef _STM_CORE_H_
# error "must be compiled via stmgc.c"
#endif


static void pages_initialize_shared(uintptr_t pagenum, uintptr_t count)
{
    /* call remap_file_pages() to make all pages in the
       range(pagenum, pagenum+count) refer to the same
       physical range of pages from region 0 */
    long i;
    for (i = 1; i < NB_REGIONS; i++) {
        char *region_base = get_region_base(i);
        int res = remap_file_pages(region_base + pagenum * 4096UL,
                                   count * 4096UL,
                                   0, pagenum, 0);
        if (res != 0) {
            perror("remap_file_pages");
            abort();
        }
    }
    for (; count > 0; count--) {
        flag_page_private[pagenum++] = SHARED_PAGE;
    }
}

static void _pages_privatize(uintptr_t pagenum, uintptr_t count)
{
    abort();
}
