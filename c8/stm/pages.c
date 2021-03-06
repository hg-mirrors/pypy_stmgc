#ifndef _STM_CORE_H_
# error "must be compiled via stmgc.c"
#endif

#include <unistd.h>
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

    char *res = mmap(addr, size,
                     PROT_READ | PROT_WRITE,
                     (MAP_PAGES_FLAGS & ~MAP_ANONYMOUS) | MAP_FIXED,
                     stm_object_pages_fd, pgoff * 4096UL);
    if (UNLIKELY(res != addr))
        stm_fatalerror("mmap (remapping page): %m");
}


static void pages_initialize_shared(uintptr_t pagenum, uintptr_t count)
{
    /* call remap_file_pages() to make all pages in the range(pagenum,
       pagenum+count) refer to the same physical range of pages from
       segment 0. */
    dprintf(("pages_initialize_shared: 0x%ld - 0x%ld\n", pagenum,
             pagenum + count));
#ifndef NDEBUG
    long l;
    for (l = 0; l < NB_SEGMENTS; l++) {
        assert(get_priv_segment(l)->privatization_lock);
    }
#endif
    assert(pagenum < NB_PAGES);
    if (count == 0)
        return;
    uintptr_t i;
    for (i = 1; i < NB_SEGMENTS; i++) {
        char *segment_base = get_segment_base(i);
        d_remap_file_pages(segment_base + pagenum * 4096UL,
                           count * 4096UL, pagenum);
    }

    for (i = 0; i < NB_SEGMENTS; i++) {
        uintptr_t amount = count;
        while (amount-->0) {
            volatile struct page_shared_s *ps2 = (volatile struct page_shared_s *)
                &pages_privatized[pagenum + amount - PAGE_FLAG_START];

            ps2->by_segment = 0; /* not private */
        }
    }
}


static void page_privatize_in(int segnum, uintptr_t pagenum, char *initialize_from)
{
#ifndef NDEBUG
    long l;
    for (l = 0; l < NB_SEGMENTS; l++) {
        assert(get_priv_segment(l)->privatization_lock);
    }
#endif

    /* check this thread's 'pages_privatized' bit */
    uint64_t bitmask = 1UL << segnum;
    volatile struct page_shared_s *ps = (volatile struct page_shared_s *)
        &pages_privatized[pagenum - PAGE_FLAG_START];
    if (ps->by_segment & bitmask) {
        /* the page is already privatized; nothing to do */
        return;
    }

    dprintf(("page_privatize(%lu) in seg:%d\n", pagenum, segnum));

    /* add this thread's 'pages_privatized' bit */
    ps->by_segment |= bitmask;

    /* "unmaps" the page to make the address space location correspond
       again to its underlying file offset (XXX later we should again
       attempt to group together many calls to d_remap_file_pages() in
       succession) */
    uintptr_t pagenum_in_file = NB_PAGES * segnum + pagenum;
    char *new_page = stm_object_pages + pagenum_in_file * 4096UL;

    /* first write to the file page directly: */
    ssize_t written = pwrite(stm_object_pages_fd, initialize_from, 4096UL,
                             pagenum_in_file * 4096UL);
    if (written != 4096)
        stm_fatalerror("pwrite didn't write the whole page: %zd", written);

    /* now remap virtual page in segment to the new file page */
    write_fence();
    d_remap_file_pages(new_page, 4096, pagenum_in_file);
}
