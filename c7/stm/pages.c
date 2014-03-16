#ifndef _STM_CORE_H_
# error "must be compiled via stmgc.c"
#endif


/************************************************************/

static union {
    struct {
        uint8_t mutex_pages;
        volatile bool major_collection_requested;
        uint64_t total_allocated;  /* keep track of how much memory we're
                                      using, ignoring nurseries */
        uint64_t total_allocated_bound;
    };
    char reserved[64];
} pages_ctl __attribute__((aligned(64)));


static void setup_pages(void)
{
    pages_ctl.total_allocated_bound = GC_MIN;
}

static void teardown_pages(void)
{
    memset(&pages_ctl, 0, sizeof(pages_ctl));
}

static void mutex_pages_lock(void)
{
    while (__sync_lock_test_and_set(&pages_ctl.mutex_pages, 1) != 0) {
        spin_loop();
    }
}

static void mutex_pages_unlock(void)
{
    __sync_lock_release(&pages_ctl.mutex_pages);
}

__attribute__((unused))
static bool _has_mutex_pages(void)
{
    return pages_ctl.mutex_pages != 0;
}

static uint64_t increment_total_allocated(ssize_t add_or_remove)
{
    pages_ctl.total_allocated += add_or_remove;

    if (pages_ctl.total_allocated >= pages_ctl.total_allocated_bound)
        pages_ctl.major_collection_requested = true;

    return pages_ctl.total_allocated;
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


static void d_remap_file_pages(char *addr, size_t size, ssize_t pgoff)
{
    dprintf(("remap_file_pages: 0x%lx bytes: (seg%ld %p) --> (seg%ld %p)\n",
             (long)size,
             (long)((addr - stm_object_pages) / 4096UL) / NB_PAGES,
             (void *)((addr - stm_object_pages) % (4096UL * NB_PAGES)),
             (long)pgoff / NB_PAGES,
             (void *)((pgoff % NB_PAGES) * 4096UL)));

    int res = remap_file_pages(addr, size, 0, pgoff, 0);
    if (UNLIKELY(res < 0))
        stm_fatalerror("remap_file_pages: %m\n");
}

static void pages_initialize_shared(uintptr_t pagenum, uintptr_t count)
{
    /* call remap_file_pages() to make all pages in the range(pagenum,
       pagenum+count) refer to the same physical range of pages from
       segment 0. */
    uintptr_t i;
    assert(_has_mutex_pages());
    for (i = 1; i <= NB_SEGMENTS; i++) {
        char *segment_base = get_segment_base(i);
        d_remap_file_pages(segment_base + pagenum * 4096UL,
                           count * 4096UL, pagenum);
    }
}

static void page_privatize(uintptr_t pagenum)
{
    wlog_t *item;
    TREE_FIND(*STM_PSEGMENT->private_page_mapping, pagenum, item,
              goto not_found);

    /* the page is already privatized */
    return;

 not_found:;
    /* look up the next free page */
    uintptr_t free_page_num = STM_PSEGMENT->private_free_page_num;

    /* "mount" it in the segment
       (XXX later we should again attempt to group together many calls to
       d_remap_file_pages() in succession) */
    char *new_page = STM_SEGMENT->segment_base + pagenum * 4096UL;
    d_remap_file_pages(new_page, 4096,
                       NB_PAGES * STM_SEGMENT->segment_num + free_page_num);
    increment_total_allocated(4096);

    /* update private_free_page_num */
    uintptr_t future_page = *(uintptr_t *)new_page;
    if (future_page == 0) {
        future_page = free_page_num + 1;
    }
    STM_PSEGMENT->private_free_page_num = future_page;

    /* copy the content from the shared (segment 0) source */
    pagecopy(new_page, stm_object_pages + pagenum * 4096UL);

    /* update private_page_mapping */
    tree_insert(STM_PSEGMENT->private_page_mapping, pagenum, free_page_num);
}

#if 0
static bool is_fully_in_shared_pages(object_t *obj)
{
    uintptr_t first_page = ((uintptr_t)obj) / 4096UL;

    if ((obj->stm_flags & GCFLAG_SMALL_UNIFORM) != 0)
        return (flag_page_private[first_page] == SHARED_PAGE);

    ssize_t obj_size = stmcb_size_rounded_up(
        (struct object_s *)REAL_ADDRESS(stm_object_pages, obj));

    uintptr_t last_page = (((uintptr_t)obj) + obj_size - 1) / 4096UL;

    do {
        if (flag_page_private[first_page++] != SHARED_PAGE)
            return false;
    } while (first_page <= last_page);

    return true;
}
#endif
