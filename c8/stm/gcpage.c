#ifndef _STM_CORE_H_
# error "must be compiled via stmgc.c"
#endif


static void setup_gcpage(void)
{
    uninitialized_page_start = stm_object_pages + END_NURSERY_PAGE * 4096UL;
    uninitialized_page_stop  = uninitialized_page_start + NB_SHARED_PAGES * 4096UL;
}

static void teardown_gcpage(void)
{
}

static void setup_N_pages(char *pages_addr, uint64_t num)
{
    /* initialize to |N|P|N|N| */
    acquire_all_privatization_locks();

    uintptr_t p = (pages_addr - stm_object_pages) / 4096UL;
    dprintf(("setup_N_pages(%p, %lu): pagenum %lu\n", pages_addr, num, p));
    while (num-->0) {
        page_mark_accessible(STM_SEGMENT->segment_num, p + num);
    }

    release_all_privatization_locks();
}


static int lock_growth_large = 0;

static stm_char *allocate_outside_nursery_large(uint64_t size)
{
    /* XXX: real allocation */
    spinlock_acquire(lock_growth_large);
    char *addr = uninitialized_page_start;

    char *start = uninitialized_page_start;
    if (addr + size > start) {  /* XXX: always for now */
        uintptr_t npages;
        npages = (addr + size - start) / 4096UL + 1;
        if (uninitialized_page_stop - start < npages * 4096UL) {
            stm_fatalerror("out of memory!");   /* XXX */
        }
        setup_N_pages(start, npages);
        if (!__sync_bool_compare_and_swap(&uninitialized_page_start,
                                          start,
                                          start + npages * 4096UL)) {
            stm_fatalerror("uninitialized_page_start changed?");
        }
    }

    dprintf(("allocate_outside_nursery_large(%lu): %p, page=%lu\n",
             size, addr,
             (uintptr_t)addr / 4096UL + END_NURSERY_PAGE));

    spinlock_release(lock_growth_large);
    return (stm_char*)(addr - stm_object_pages);
}

object_t *_stm_allocate_old(ssize_t size_rounded_up)
{
    /* only for tests xxx but stm_setup_prebuilt() uses this now too */
    stm_char *p = allocate_outside_nursery_large(size_rounded_up);
    /* hardcode segment 1 */
    memset(get_virtual_address(STM_SEGMENT->segment_num, (object_t *)p), 0, size_rounded_up);

    object_t *o = (object_t *)p;
    o->stm_flags = GCFLAG_WRITE_BARRIER;

    dprintf(("allocate_old(%lu): %p, seg=%d, page=%lu\n",
             size_rounded_up, p,
             get_segment_of_linear_address(stm_object_pages + (uintptr_t)p),
             (uintptr_t)p / 4096UL));
    return o;
}
