#ifndef _STM_CORE_H_
# error "must be compiled via stmgc.c"
#endif


/************************************************************/

static void setup_pages(void)
{
}

static void teardown_pages(void)
{
    memset(pages_privatized, 0, sizeof(pages_privatized));
}

/************************************************************/

static void d_remap_file_pages(char *addr, size_t size, ssize_t pgoff)
{
    dprintf(("remap_file_pages: 0x%lx bytes: (seg%ld %p) --> (seg%ld %p)\n",
             (long)size,
             (long)((addr - stm_object_pages) / 4096UL) / NB_PAGES,
             (void *)((addr - stm_object_pages) % (4096UL * NB_PAGES)),
             (long)pgoff / NB_PAGES,
             (void *)((pgoff % NB_PAGES) * 4096UL)));
    assert(size % 4096 == 0);
    assert(size <= TOTAL_MEMORY);
    assert(((uintptr_t)addr) % 4096 == 0);
    assert(addr >= stm_object_pages);
    assert(addr <= stm_object_pages + TOTAL_MEMORY - size);
    assert(pgoff >= 0);
    assert(pgoff <= (TOTAL_MEMORY - size) / 4096UL);

    /* assert remappings follow the rule that page N in one segment
       can only be remapped to page N in another segment */
    assert(((addr - stm_object_pages) / 4096UL - pgoff) % NB_PAGES == 0);

#ifdef USE_REMAP_FILE_PAGES
    int res = remap_file_pages(addr, size, 0, pgoff, 0);
    if (UNLIKELY(res < 0))
        stm_fatalerror("remap_file_pages: %m");
#else
    char *res = mmap(addr, size,
                     PROT_READ | PROT_WRITE,
                     (MAP_PAGES_FLAGS & ~MAP_ANONYMOUS) | MAP_FIXED,
                     stm_object_pages_fd, pgoff * 4096UL);
    if (UNLIKELY(res != addr))
        stm_fatalerror("mmap (remapping page): %m");
#endif
}


static void pages_initialize_shared(uintptr_t pagenum, uintptr_t count)
{
    /* call remap_file_pages() to make all pages in the range(pagenum,
       pagenum+count) refer to the same physical range of pages from
       segment 0. */
    dprintf(("pages_initialize_shared: 0x%ld - 0x%ld\n", pagenum,
             pagenum + count));
    assert(pagenum < NB_PAGES);
    if (count == 0)
        return;
    uintptr_t i;
    for (i = 1; i < NB_SEGMENTS; i++) {
        char *segment_base = get_segment_base(i);
        d_remap_file_pages(segment_base + pagenum * 4096UL,
                           count * 4096UL, pagenum);
    }
}
