#ifndef _STM_CORE_H_
# error "must be compiled via stmgc.c"
# include "core.h"  // silence flymake
#endif

#include <unistd.h>
/************************************************************/
struct {
    volatile bool major_collection_requested;
    uint64_t total_allocated;  /* keep track of how much memory we're
                                  using, ignoring nurseries */
    uint64_t total_allocated_bound;
} pages_ctl;


static void setup_pages(void)
{
    pages_ctl.total_allocated_bound = GC_MIN;
}

static void teardown_pages(void)
{
    memset(&pages_ctl, 0, sizeof(pages_ctl));
    memset(pages_status, 0, sizeof(pages_status));
}

static uint64_t increment_total_allocated(ssize_t add_or_remove)
{
    uint64_t ta = __sync_add_and_fetch(&pages_ctl.total_allocated,
                                       add_or_remove);

    if (UNLIKELY(ta >= pages_ctl.total_allocated_bound))
        pages_ctl.major_collection_requested = true;

    return ta;
}

static bool is_major_collection_requested(void)
{
    return pages_ctl.major_collection_requested;
}

static void force_major_collection_request(void)
{
    pages_ctl.major_collection_requested = true;
}

static void reset_major_collection_requested(void)
{
    assert(_has_mutex());

    uint64_t next_bound = (uint64_t)((double)pages_ctl.total_allocated *
                                     GC_MAJOR_COLLECT);
    if (next_bound < GC_MIN)
        next_bound = GC_MIN;

    pages_ctl.total_allocated_bound = next_bound;
    pages_ctl.major_collection_requested = false;
}


/************************************************************/


static void page_mark_accessible(long segnum, uintptr_t pagenum)
{
#ifndef NDEBUG
    uint8_t page_status = get_page_status_in(segnum, pagenum);
    assert(segnum==0
           || page_status == PAGE_NO_ACCESS
           || page_status == PAGE_READONLY);
#endif

    dprintf(("page_mark_accessible(%lu) in seg:%ld\n", pagenum, segnum));

    if (mprotect(get_virtual_page(segnum, pagenum), 4096, PROT_READ | PROT_WRITE)) {
        perror("mprotect");
        stm_fatalerror("mprotect failed! Consider running 'sysctl vm.max_map_count=16777216'");
    }

    /* set this flag *after* we un-protected it, because XXX later */
    set_page_status_in(segnum, pagenum, PAGE_ACCESSIBLE);
    set_hint_modified_recently(pagenum);
    dprintf(("RW(seg%ld, page %lu)\n", segnum, pagenum));
}

static void page_mark_inaccessible(long segnum, uintptr_t pagenum)
{
    assert(segnum==0
           || get_page_status_in(segnum, pagenum) == PAGE_ACCESSIBLE
           || get_page_status_in(segnum, pagenum) == PAGE_READONLY);
    dprintf(("page_mark_inaccessible(%lu) in seg:%ld\n", pagenum, segnum));

    set_page_status_in(segnum, pagenum, PAGE_NO_ACCESS);

    dprintf(("NONE(seg%ld, page %lu)\n", segnum, pagenum));
    char *addr = get_virtual_page(segnum, pagenum);
    madvise(addr, 4096, MADV_DONTNEED);
    if (mprotect(addr, 4096, PROT_NONE)) {
        perror("mprotect");
        stm_fatalerror("mprotect failed! Consider running 'sysctl vm.max_map_count=16777216'");
    }
}


static void page_mark_readonly(long segnum, uintptr_t pagenum)
{
    /* mark readonly and share with seg0 */
    assert(segnum > 0 &&
           (get_page_status_in(segnum, pagenum) == PAGE_ACCESSIBLE
            || get_page_status_in(segnum, pagenum) == PAGE_NO_ACCESS));
    dprintf(("page_mark_readonly(%lu) in seg:%ld\n", pagenum, segnum));

    char *virt_addr = get_virtual_page(segnum, pagenum);
    madvise(virt_addr, 4096UL, MADV_DONTNEED);
    /* XXX: does it matter if SHARED or PRIVATE?
       IF MAP_SHARED, make sure page_mark_accessible doesn't simply mprotect() but also
       mmap() as MAP_PRIVATE */
    /* XXX: MAP_PRIVATE should work because AFAIK when we map PRIVATE to a
       SHARED file page, we should really share it in memory until we write
       to it, in which case we do all necessary things in the segfault handler. */
    char *res = mmap(virt_addr, 4096UL,
                     PROT_READ,
                     MAP_PRIVATE|MAP_FIXED|MAP_NORESERVE,
                     stm_object_pages_fd,
                     (pagenum - END_NURSERY_PAGE) * 4096UL);
    if (res == MAP_FAILED)
        stm_fatalerror("%s failed (mmap): %m", "page_mark_readonly");

    set_page_status_in(segnum, pagenum, PAGE_READONLY);

    dprintf(("RO(seg%ld, page %lu)\n", segnum, pagenum));
}
