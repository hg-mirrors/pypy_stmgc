#ifndef _STM_CORE_H_
# error "must be compiled via stmgc.c"
#endif


static void pages_initialize_shared(uintptr_t pagenum, uintptr_t count)
{
    /* call remap_file_pages() to make all pages in the
       range(pagenum, pagenum+count) refer to the same
       physical range of pages from segment 0 */
    long i;
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

static void _pages_privatize(uintptr_t pagenum, uintptr_t count)
{
    assert(count == 1);   /* XXX */

#ifdef HAVE_FULL_EXCHANGE_INSN
    /* use __sync_lock_test_and_set() as a cheaper alternative to
       __sync_bool_compare_and_swap(). */
    int previous = __sync_lock_test_and_set(&flag_page_private[pagenum],
                                            REMAPPING_PAGE);
    assert(previous != FREE_PAGE);
    if (previous == PRIVATE_PAGE) {
        flag_page_private[pagenum] = PRIVATE_PAGE;
        return;
    }
    bool was_shared = (previous == SHARED_PAGE);
#else
    bool was_shared = __sync_bool_compare_and_swap(&flag_page_private[pagenum],
                                                  SHARED_PAGE, REMAPPING_PAGE);
#endif
    if (!was_shared) {
        while (1) {
            uint8_t state = ((uint8_t volatile *)flag_page_private)[pagenum];
            if (state != REMAPPING_PAGE) {
                assert(state == PRIVATE_PAGE);
                break;
            }
            spin_loop();
        }
        return;
    }

    ssize_t pgoff1 = pagenum;
    ssize_t pgoff2 = pagenum + NB_PAGES;
    ssize_t localpgoff = pgoff1 + NB_PAGES * STM_SEGMENT->segment_num;
    ssize_t otherpgoff = pgoff1 + NB_PAGES * (1 - STM_SEGMENT->segment_num);

    void *localpg = stm_object_pages + localpgoff * 4096UL;
    void *otherpg = stm_object_pages + otherpgoff * 4096UL;

    // XXX should not use pgoff2, but instead the next unused page in
    // thread 2, so that after major GCs the next dirty pages are the
    // same as the old ones
    int res = remap_file_pages(localpg, 4096, 0, pgoff2, 0);
    if (res < 0) {
        perror("remap_file_pages");
        abort();
    }
    pagecopy(localpg, otherpg);
    write_fence();
    assert(flag_page_private[pagenum] == REMAPPING_PAGE);
    flag_page_private[pagenum] = PRIVATE_PAGE;
}
