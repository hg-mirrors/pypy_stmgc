#ifndef _STM_CORE_H_
# error "must be compiled via stmgc.c"
#endif


static void pages_initialize_shared(uintptr_t pagenum, uintptr_t count)
{
    /* call remap_file_pages() to make all pages in the
       range(pagenum, pagenum+count) refer to the same
       physical range of pages from segment 0 */
    uintptr_t i;
    for (i = 1; i < NB_SEGMENTS; i++) {
        char *segment_base = get_segment_base(i);
        int res = remap_file_pages(segment_base + pagenum * 4096UL,
                                   count * 4096UL,
                                   0, pagenum, 0);
        if (res != 0) {
            perror("remap_file_pages");
            abort();
        }
    }
    for (i = 0; i < count; i++) {
        assert(flag_page_private[pagenum + i] == FREE_PAGE);
        flag_page_private[pagenum + i] = SHARED_PAGE;
    }
}

static void privatize_range_and_unlock(uintptr_t pagenum, uintptr_t count,
                                       bool full)
{
    ssize_t pgoff1 = pagenum;
    ssize_t pgoff2 = pagenum + NB_PAGES;
    ssize_t localpgoff = pgoff1 + NB_PAGES * STM_SEGMENT->segment_num;
    ssize_t otherpgoff = pgoff1 + NB_PAGES * (1 - STM_SEGMENT->segment_num);

    void *localpg = stm_object_pages + localpgoff * 4096UL;
    void *otherpg = stm_object_pages + otherpgoff * 4096UL;

    int res = remap_file_pages(localpg, count * 4096, 0, pgoff2, 0);
    if (res < 0) {
        perror("remap_file_pages");
        abort();
    }
    uintptr_t i;
    if (full) {
        for (i = 0; i < count; i++) {
            pagecopy(localpg + 4096 * i, otherpg + 4096 * i);
        }
    }
    else {
        pagecopy(localpg, otherpg);
        if (count > 1)
            pagecopy(localpg + 4096 * (count-1), otherpg + 4096 * (count-1));
    }
    write_fence();
    for (i = 0; i < count; i++) {
        assert(flag_page_private[pagenum + i] == REMAPPING_PAGE);
        flag_page_private[pagenum + i] = PRIVATE_PAGE;
    }
}

static void _pages_privatize(uintptr_t pagenum, uintptr_t count, bool full)
{
    uintptr_t page_start_range = pagenum;
    uintptr_t pagestop = pagenum + count;

    while (flag_page_private[pagenum + count - 1] == PRIVATE_PAGE) {
        if (!--count)
            return;
    }

    for (; pagenum < pagestop; pagenum++) {
#ifdef HAVE_FULL_EXCHANGE_INSN
        /* use __sync_lock_test_and_set() as a cheaper alternative to
           __sync_bool_compare_and_swap(). */
        int prev = __sync_lock_test_and_set(&flag_page_private[pagenum],
                                            REMAPPING_PAGE);
        assert(prev != FREE_PAGE);
        if (prev == PRIVATE_PAGE) {
            flag_page_private[pagenum] = PRIVATE_PAGE;
        }
        bool was_shared = (prev == SHARED_PAGE);
#else
        bool was_shared = __sync_bool_compare_and_swap(
                                            &flag_page_private[pagenum + cnt1],
                                            SHARED_PAGE, REMAPPING_PAGE);
#endif
        if (!was_shared) {
            if (pagenum > page_start_range) {
                privatize_range_and_unlock(page_start_range,
                                           pagenum - page_start_range, full);
            }
            page_start_range = pagenum + 1;

            while (1) {
                uint8_t state;
                state = ((uint8_t volatile *)flag_page_private)[pagenum];
                if (state != REMAPPING_PAGE) {
                    assert(state == PRIVATE_PAGE);
                    break;
                }
                spin_loop();
            }
        }
    }

    if (pagenum > page_start_range) {
        privatize_range_and_unlock(page_start_range,
                                   pagenum - page_start_range, full);
    }
}

static void set_creation_markers(stm_char *p, uint64_t size, int newvalue)
{
    /* Set the creation markers to 'newvalue' for all lines from 'p' to
       'p+size'.  Both p and size should be aligned to the line size: 256. */

    assert((((uintptr_t)p) & 255) == 0);
    assert((size & 255) == 0);
    assert(size > 0);

    uintptr_t cmaddr = ((uintptr_t)p) >> 8;
    LIST_APPEND(STM_PSEGMENT->creation_markers, cmaddr);

    char *addr = REAL_ADDRESS(STM_SEGMENT->segment_base, cmaddr);
    memset(addr, newvalue, size >> 8);
}

static uint8_t get_single_creation_marker(stm_char *p)
{
    uintptr_t cmaddr = ((uintptr_t)p) >> 8;
    return ((stm_creation_marker_t *)cmaddr)->cm;
}

static void set_single_creation_marker(stm_char *p, int newvalue)
{
    uintptr_t cmaddr = ((uintptr_t)p) >> 8;
    ((stm_creation_marker_t *)cmaddr)->cm = newvalue;
    LIST_APPEND(STM_PSEGMENT->creation_markers, cmaddr);
}

static void reset_all_creation_markers(void)
{
    /* Note that the page 'NB_PAGES - 1' is not actually used.  This
       ensures that the creation markers always end with some zeroes.
       We reset the markers 8 at a time, by writing null integers
       until we reach a place that is already null.
    */
    LIST_FOREACH_R(
        STM_PSEGMENT->creation_markers,
        uintptr_t /*item*/,
        ({
            TLPREFIX uint64_t *p = (TLPREFIX uint64_t *)(item & ~7);
            while (*p != 0)
                *p++ = 0;
        }));

    list_clear(STM_PSEGMENT->creation_markers);
}

static void reset_all_creation_markers_and_push_created_data(void)
{
    /* This is like reset_all_creation_markers(), but additionally
       it looks for markers in non-SHARED pages, and pushes the
       corresponding data (in 256-bytes blocks) to other threads.
    */
#if NB_SEGMENTS != 2
# error "The logic in this function only works with two segments"
#endif

    char *local_base = STM_SEGMENT->segment_base;
    long remote_num = 1 - STM_SEGMENT->segment_num;
    char *remote_base = get_segment_base(remote_num);

    /* this logic assumes that creation markers are in 256-bytes blocks,
       and pages are 4096 bytes, so creation markers are handled by groups
       of 16 --- which is two 8-bytes uint64_t. */

    LIST_FOREACH_R(
        STM_PSEGMENT->creation_markers,
        uintptr_t /*item*/,
        ({
            TLPREFIX uint64_t *p = (TLPREFIX uint64_t *)(item & ~15);
            while (p[0] != 0 || p[1] != 0) {

                uint64_t pagenum = ((uint64_t)p) >> 4;
                if (flag_page_private[pagenum] != SHARED_PAGE) {
                    /* copying needed */
                    uint64_t dataofs = ((uint64_t)p) << 8;
                    stm_char *start = (stm_char *)p;
                    stm_char *stop = start + 16;
                    while (start < stop) {
                        if (*start++ != 0) {
                            pagecopy_256(remote_base + dataofs,
                                         local_base + dataofs);
                        }
                        dataofs += 256;
                    }
                }
                p[0] = 0; _duck();
                p[1] = 0;
                p += 2;
            }
        }));

    list_clear(STM_PSEGMENT->creation_markers);
}
