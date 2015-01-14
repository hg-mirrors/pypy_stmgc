#ifndef _STM_CORE_H_
# error "must be compiled via stmgc.c"
#endif

static struct list_s *testing_prebuilt_objs = NULL;
static struct tree_s *tree_prebuilt_objs = NULL;     /* XXX refactor */


static void setup_gcpage(void)
{
    char *base = stm_object_pages + END_NURSERY_PAGE * 4096UL;
    uintptr_t length = (NB_PAGES - END_NURSERY_PAGE) * 4096UL;
    _stm_largemalloc_init_arena(base, length);

    uninitialized_page_start = stm_object_pages + END_NURSERY_PAGE * 4096UL;
    uninitialized_page_stop  = uninitialized_page_start + NB_SHARED_PAGES * 4096UL;
}

static void teardown_gcpage(void)
{
    LIST_FREE(testing_prebuilt_objs);
    if (tree_prebuilt_objs != NULL) {
        tree_free(tree_prebuilt_objs);
        tree_prebuilt_objs = NULL;
    }
}



static void setup_N_pages(char *pages_addr, long num)
{
    /* make pages accessible in sharing segment only (pages already
       PROT_READ/WRITE (see setup.c), but not marked accessible as page
       status). */

    /* lock acquiring maybe not necessary because the affected pages don't
       need privatization protection. (but there is an assert right
       now to enforce that XXXXXX) */
    acquire_all_privatization_locks();

    uintptr_t p = (pages_addr - stm_object_pages) / 4096UL;
    dprintf(("setup_N_pages(%p, %lu): pagenum %lu\n", pages_addr, num, p));
    while (num-->0) {
        /* XXX: page_range_mark_accessible() */
        page_mark_accessible(0, p + num);
    }

    release_all_privatization_locks();
}


static int lock_growth_large = 0;

static stm_char *allocate_outside_nursery_large(uint64_t size)
{
    /* Allocate the object with largemalloc.c from the lower addresses. */
    char *addr = _stm_large_malloc(size);
    if (addr == NULL)
        stm_fatalerror("not enough memory!");

    if (LIKELY(addr + size <= uninitialized_page_start))
        return (stm_char*)(addr - stm_object_pages);


    /* uncommon case: need to initialize some more pages */
    spinlock_acquire(lock_growth_large);

    char *start = uninitialized_page_start;
    if (addr + size > start) {
        uintptr_t npages;
        npages = (addr + size - start) / 4096UL;
        npages += GCPAGE_NUM_PAGES;
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
             size, (char*)(addr - stm_object_pages),
             (uintptr_t)(addr - stm_object_pages) / 4096UL));

    spinlock_release(lock_growth_large);
    return (stm_char*)(addr - stm_object_pages);
}

object_t *_stm_allocate_old(ssize_t size_rounded_up)
{
    /* only for tests xxx but stm_setup_prebuilt() uses this now too */
    stm_char *p = allocate_outside_nursery_large(size_rounded_up);
    object_t *o = (object_t *)p;

    memset(get_virtual_address(STM_SEGMENT->segment_num, o), 0, size_rounded_up);
    o->stm_flags = GCFLAG_WRITE_BARRIER;

    dprintf(("allocate_old(%lu): %p, seg=%d, page=%lu\n",
             size_rounded_up, p,
             get_segment_of_linear_address(stm_object_pages + (uintptr_t)p),
             (uintptr_t)p / 4096UL));
    return o;
}
