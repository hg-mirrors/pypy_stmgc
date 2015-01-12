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


static void page_mark_accessible(long segnum, uintptr_t pagenum)
{
    assert(get_page_status_in(segnum, pagenum) == PAGE_NO_ACCESS);
    dprintf(("page_mark_accessible(%lu) in seg:%ld\n", pagenum, segnum));

    mprotect(get_virtual_page(segnum, pagenum), 4096, PROT_READ | PROT_WRITE);

    /* set this flag *after* we un-protected it, because XXX later */
    set_page_status_in(segnum, pagenum, PAGE_ACCESSIBLE);
}

__attribute__((unused))
static void page_mark_inaccessible(long segnum, uintptr_t pagenum)
{
    assert(get_page_status_in(segnum, pagenum) == PAGE_ACCESSIBLE);
    dprintf(("page_mark_inaccessible(%lu) in seg:%ld\n", pagenum, segnum));

    set_page_status_in(segnum, pagenum, PAGE_ACCESSIBLE);

    char *addr = get_virtual_page(segnum, pagenum);
    madvise(get_virtual_page(segnum, pagenum), 4096, MADV_DONTNEED);
    mprotect(addr, 4096, PROT_NONE);
}
