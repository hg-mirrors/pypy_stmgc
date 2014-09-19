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
    memset(pages_status, 0, sizeof(pages_status));
}

/************************************************************/
static void check_remap_makes_sense(char *addr, size_t size, ssize_t pgoff)
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
}

static void d_remap_file_pages(char *addr, size_t size, ssize_t pgoff)
{
    check_remap_makes_sense(addr, size, pgoff);

    char *res = mmap(addr, size,
                     PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_NORESERVE | MAP_FIXED,
                     stm_object_pages_fd, pgoff * 4096UL);
    if (UNLIKELY(res != addr))
        stm_fatalerror("mmap (remapping page): %m");
}


static void pages_initialize_shared_for(long segnum, uintptr_t pagenum, uintptr_t count)
{
    /* call remap_file_pages() to make all pages in the range(pagenum,
       pagenum+count) PAGE_SHARED in segnum, and PAGE_NO_ACCESS in other segments */

    dprintf(("pages_initialize_shared: 0x%ld - 0x%ld\n", pagenum, pagenum + count));

    assert(all_privatization_locks_acquired());

    assert(pagenum < NB_PAGES);
    if (count == 0)
        return;

    /* already shared after setup.c (also for the other count-1 pages) */
    assert(get_page_status_in(segnum, pagenum) == PAGE_SHARED);

    /* make other segments NO_ACCESS: */
    uintptr_t i;
    for (i = 0; i < NB_SEGMENTS; i++) {
        if (i != segnum) {
            char *segment_base = get_segment_base(i);
            mprotect(segment_base + pagenum * 4096UL,
                     count * 4096UL, PROT_NONE);
        }

        long amount = count;
        while (amount-->0)
            set_page_status_in(i, pagenum + amount, PAGE_NO_ACCESS);
    }
}


static void page_privatize_in(int segnum, uintptr_t pagenum)
{
#ifndef NDEBUG
    long l;
    for (l = 0; l < NB_SEGMENTS; l++) {
        assert(get_priv_segment(l)->privatization_lock);
    }
#endif
    assert(get_page_status_in(segnum, pagenum) == PAGE_NO_ACCESS);
    dprintf(("page_privatize(%lu) in seg:%d\n", pagenum, segnum));

    char *addr = (char*)(get_virt_page_of(segnum, pagenum) * 4096UL);
    char *result = mmap(
        addr, 4096UL, PROT_READ | PROT_WRITE,
        MAP_FIXED | MAP_PRIVATE | MAP_NORESERVE,
        stm_object_pages_fd, get_file_page_of(pagenum));
    if (result == MAP_FAILED)
        stm_fatalerror("page_privatize_in failed (mmap): %m");

    set_page_status_in(segnum, pagenum, PAGE_PRIVATE);
}


static void memcpy_to_accessible_pages(
    int dst_segnum, object_t *dst_obj, char *src, size_t len)
{
    /* XXX: optimize */

    char *realobj = REAL_ADDRESS(get_segment_base(dst_segnum), dst_obj);
    char *dst_end = realobj + len;
    uintptr_t loc_addr = (uintptr_t)dst_obj;

    while (realobj != dst_end) {
        if (get_page_status_in(dst_segnum, loc_addr / 4096UL) != PAGE_NO_ACCESS)
            *realobj = *src;
        realobj++;
        loc_addr++;
        src++;
    }
}
