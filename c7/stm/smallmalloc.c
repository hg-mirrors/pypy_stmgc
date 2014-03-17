#ifndef _STM_CORE_H_
# error "must be compiled via stmgc.c"
#endif


static void teardown_smallmalloc(void)
{
    memset(small_page_lists, 0, sizeof(small_page_lists));
    assert(free_uniform_pages == NULL);
    first_small_uniform_loc = (uintptr_t) -1;
}

static void grab_more_free_pages_for_small_allocations(void)
{
    /* Grab GCPAGE_NUM_PAGES pages out of the top addresses.  Use the
       lock of pages.c to prevent any remapping from occurring under our
       feet.
    */
    mutex_pages_lock();

    if (free_uniform_pages == NULL) {

        uintptr_t decrease_by = GCPAGE_NUM_PAGES * 4096;
        if (uninitialized_page_stop - uninitialized_page_start < decrease_by)
            goto out_of_memory;

        uninitialized_page_stop -= decrease_by;
        first_small_uniform_loc = (uintptr_t)uninitialized_page_stop;

        char *base = stm_object_pages + END_NURSERY_PAGE * 4096UL;
        if (!_stm_largemalloc_resize_arena(uninitialized_page_stop - base))
            goto out_of_memory;

        setup_N_pages(uninitialized_page_stop, GCPAGE_NUM_PAGES);

        char *p = uninitialized_page_stop;
        long i;
        for (i = 0; i < GCPAGE_NUM_PAGES; i++) {
            ((struct small_page_list_s *)p)->nextpage = free_uniform_pages;
            free_uniform_pages = (struct small_page_list_s *)p;
            p += 4096;
        }
    }

    mutex_pages_unlock();
    return;

 out_of_memory:
    stm_fatalerror("out of memory!\n");   /* XXX */
}

static char *_allocate_small_slowpath(uint64_t size)
{
    /* First try to grab the next page from the global 'small_page_list'
     */
    long n = size / 8;
    struct small_page_list_s *smallpage;
    struct small_free_loc_s *TLPREFIX *fl =
        &STM_PSEGMENT->small_malloc_data.loc_free[n];
    assert(*fl == NULL);

 retry:
    smallpage = small_page_lists[n];
    if (smallpage != NULL) {
        if (UNLIKELY(!__sync_bool_compare_and_swap(&small_page_lists[n],
                                                   smallpage,
                                                   smallpage->nextpage)))
            goto retry;

        /* Succeeded: we have a page in 'smallpage' */
        *fl = smallpage->header.next;
        return (char *)smallpage;
    }

    /* There is no more page waiting.  Maybe we can pick one from
       free_uniform_pages.
     */
    smallpage = free_uniform_pages;
    if (smallpage != NULL) {
        if (UNLIKELY(!__sync_bool_compare_and_swap(&free_uniform_pages,
                                                   smallpage,
                                                   smallpage->nextpage)))
            goto retry;

        /* Succeeded: we have a page in 'smallpage', which is not
           initialized so far, apart from the 'nextpage' field read
           above.  Initialize it.
        */
        assert(!(((uintptr_t)smallpage) & 4095));
        struct small_free_loc_s *p, *following = NULL;

        /* Initialize all slots from the second one to the last one to
           contain a chained list */
        uintptr_t i = size;
        while (i <= 4096 - size) {
            p = (struct small_free_loc_s *)(((char *)smallpage) + i);
            p->next = following;
            following = p;
            i += size;
        }

        /* The first slot is immediately returned */
        *fl = following;
        return (char *)smallpage;
    }

    /* Not a single free page left.  Grab some more free pges and retry. */
    grab_more_free_pages_for_small_allocations();
    goto retry;
}

__attribute__((always_inline))
static inline char *allocate_outside_nursery_small(uint64_t size)
{
    OPT_ASSERT((size & 7) == 0);
    OPT_ASSERT(16 <= size && size < 8 * GC_N_SMALL_REQUESTS);

    struct small_free_loc_s *TLPREFIX *fl =
        &STM_PSEGMENT->small_malloc_data.loc_free[size / 8];

    struct small_free_loc_s *result = *fl;

    if (UNLIKELY(result == NULL))
        return _allocate_small_slowpath(size);

    *fl = result->next;
    return (char *)result;
}
