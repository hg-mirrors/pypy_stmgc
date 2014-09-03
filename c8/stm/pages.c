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


static void pages_initialize_private(uintptr_t pagenum, uintptr_t count)
{
    dprintf(("pages_initialize_private: 0x%ld - 0x%ld\n", pagenum,
             pagenum + count));
    assert(pagenum < NB_PAGES);
    if (count == 0)
        return;

    long i;
    for (i = 0; i < NB_SEGMENTS; i++) {
        spinlock_acquire(get_priv_segment(i)->privatization_lock);
    }

    while (count-->0) {
        for (i = 0; i < NB_SEGMENTS; i++) {
             uint64_t bitmask = 1UL << i;
             volatile struct page_shared_s *ps = (volatile struct page_shared_s *)
                 &pages_privatized[pagenum + count - PAGE_FLAG_START];

             ps->by_segment |= bitmask;
        }
    }

    for (i = NB_SEGMENTS-1; i >= 0; i--) {
        spinlock_release(get_priv_segment(i)->privatization_lock);
    }
}
