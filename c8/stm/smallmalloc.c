#ifndef _STM_CORE_H_
# error "must be compiled via stmgc.c"
#endif


#define PAGE_SMSIZE_START   0
#define PAGE_SMSIZE_END     NB_SHARED_PAGES

typedef struct {
    uint8_t sz;
} fpsz_t;

static fpsz_t full_pages_object_size[PAGE_SMSIZE_END - PAGE_SMSIZE_START];
/* ^^^ This array contains the size (in number of words) of the objects
   in the given page, provided it's a "full page of small objects".  It
   is 0 if it's not such a page, if it's fully free, or if it's in
   small_page_lists.  It is not 0 as soon as the page enters the
   segment's 'small_malloc_data.loc_free' (even if the page is not
   technically full yet, it will be very soon in this case).
*/

static fpsz_t *get_fpsz(char *smallpage)
{
    uintptr_t pagenum = (((char *)smallpage) - stm_file_pages) / 4096;
    assert(PAGE_SMSIZE_START <= pagenum && pagenum < PAGE_SMSIZE_END);
    return &full_pages_object_size[pagenum - PAGE_SMSIZE_START];
}


#ifdef STM_TESTS
bool (*_stm_smallmalloc_keep)(char *data);   /* a hook for tests */
#endif

static void teardown_smallmalloc(void)
{
    memset(small_page_lists, 0, sizeof(small_page_lists));
    assert(free_uniform_pages == NULL);   /* done by the previous line */
    first_small_uniform_loc = (uintptr_t) -1;
#ifdef STM_TESTS
    _stm_smallmalloc_keep = NULL;
#endif
    memset(full_pages_object_size, 0, sizeof(full_pages_object_size));
}

static int gmfp_lock = 0;

static void grab_more_free_pages_for_small_allocations(void)
{
    dprintf(("grab_more_free_pages_for_small_allocation()\n"));
    /* Grab GCPAGE_NUM_PAGES pages out of the top addresses.  Use the
       lock of pages.c to prevent any remapping from occurring under our
       feet.
    */
    spinlock_acquire(gmfp_lock);

    if (free_uniform_pages == NULL) {

        uintptr_t decrease_by = GCPAGE_NUM_PAGES * 4096;
        if (uninitialized_page_stop - uninitialized_page_start < decrease_by)
            goto out_of_memory;

        uninitialized_page_stop -= decrease_by;
        first_small_uniform_loc = uninitialized_page_stop - stm_file_pages;

        /* XXX: */
        /* char *base = stm_object_pages + END_NURSERY_PAGE * 4096UL; */
        /* if (!_stm_largemalloc_resize_arena(uninitialized_page_stop - base)) */
        /*     goto out_of_memory; */

        setup_N_pages(uninitialized_page_stop, GCPAGE_NUM_PAGES);

        char *p = uninitialized_page_stop;
        long i;
        for (i = 0; i < GCPAGE_NUM_PAGES; i++) {
            ((struct small_free_loc_s *)p)->nextpage = free_uniform_pages;
            free_uniform_pages = (struct small_free_loc_s *)p;
            p += 4096;
        }
    }

    spinlock_release(gmfp_lock);
    return;

 out_of_memory:
    stm_fatalerror("out of memory!\n");   /* XXX */
}

static char *_allocate_small_slowpath(uint64_t size)
{
    long n = size / 8;
    struct small_free_loc_s *smallpage;
    struct small_free_loc_s *TLPREFIX *fl =
        &STM_PSEGMENT->small_malloc_data.loc_free[n];
    assert(*fl == NULL);

 retry:
    /* First try to grab the next page from the global 'small_page_list'
     */
    smallpage = small_page_lists[n];
    if (smallpage != NULL) {
        if (UNLIKELY(!__sync_bool_compare_and_swap(&small_page_lists[n],
                                                   smallpage,
                                                   smallpage->nextpage)))
            goto retry;

        /* Succeeded: we have a page in 'smallpage' */
        *fl = smallpage->next;
        get_fpsz((char *)smallpage)->sz = n;
        return (char *)smallpage;
    }

    /* There is no more page waiting for the correct size of objects.
       Maybe we can pick one from free_uniform_pages.
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
        struct small_free_loc_s *p, **previous;
        assert(!(((uintptr_t)smallpage) & 4095));
        previous = (struct small_free_loc_s **)
            REAL_ADDRESS(STM_SEGMENT->segment_base, fl);

        /* Initialize all slots from the second one to the last one to
           contain a chained list */
        uintptr_t i = size;
        while (i <= 4096 - size) {
            p = (struct small_free_loc_s *)(((char *)smallpage) + i);
            *previous = p;
            previous = &p->next;
            i += size;
        }
        *previous = NULL;

        /* The first slot is immediately returned */
        get_fpsz((char *)smallpage)->sz = n;
        return (char *)smallpage;
    }

    /* Not a single free page left.  Grab some more free pages and retry.
     */
    grab_more_free_pages_for_small_allocations();
    goto retry;
}

__attribute__((always_inline))
static inline stm_char *allocate_outside_nursery_small(uint64_t size)
{
    OPT_ASSERT((size & 7) == 0);
    OPT_ASSERT(16 <= size && size <= GC_LAST_SMALL_SIZE);

    struct small_free_loc_s *TLPREFIX *fl =
        &STM_PSEGMENT->small_malloc_data.loc_free[size / 8];

    struct small_free_loc_s *result = *fl;

    if (UNLIKELY(result == NULL))
        return (stm_char*)
            (_allocate_small_slowpath(size) - stm_file_pages + END_NURSERY_PAGE * 4096UL);

    *fl = result->next;
    return (stm_char*)
        ((char *)result - stm_file_pages + END_NURSERY_PAGE * 4096UL);
}

object_t *_stm_allocate_old_small(ssize_t size_rounded_up)
{
    stm_char *p = allocate_outside_nursery_small(size_rounded_up);
    memset(stm_object_pages + (uintptr_t)p, 0, size_rounded_up);

    object_t *o = (object_t *)p;
    o->stm_flags = GCFLAG_WRITE_BARRIER;

    dprintf(("allocate_old_small(%lu): %p, seg=%d, page=%lu\n",
             size_rounded_up, p,
             get_segment_of_linear_address(stm_object_pages + (uintptr_t)p),
             (uintptr_t)p / 4096UL));

    return o;
}

/************************************************************/

static inline bool _smallmalloc_sweep_keep(char *p)
{
#ifdef STM_TESTS
    if (_stm_smallmalloc_keep != NULL) {
        // test wants a TLPREFIXd address
        return _stm_smallmalloc_keep(
            p - stm_file_pages + (char*)(END_NURSERY_PAGE * 4096UL));
    }
#endif
    abort();
    //return smallmalloc_keep_object_at(p);
}

void check_order_inside_small_page(struct small_free_loc_s *page)
{
#ifndef NDEBUG
    /* the free locations are supposed to be in increasing order */
    while (page->next != NULL) {
        assert(page->next > page);
        page = page->next;
    }
#endif
}

static char *getbaseptr(struct small_free_loc_s *fl)
{
    return (char *)(((uintptr_t)fl) & ~4095);
}

void sweep_small_page(char *baseptr, struct small_free_loc_s *page_free,
                      long szword)
{
    if (page_free != NULL)
        check_order_inside_small_page(page_free);

    /* for every non-free location, ask if we must free it */
    uintptr_t i, size = szword * 8;
    bool any_object_remaining = false, any_object_dying = false;
    struct small_free_loc_s *fl = page_free;
    struct small_free_loc_s *flprev = NULL;

    /* XXX could optimize for the case where all objects die: we don't
       need to painfully rebuild the free list in the whole page, just
       to have it ignored in the end because we put the page into
       'free_uniform_pages' */

    for (i = 0; i <= 4096 - size; i += size) {
        char *p = baseptr + i;
        if (p == (char *)fl) {
            /* location is already free */
            flprev = fl;
            fl = fl->next;
            any_object_dying = true;
        }
        else if (!_smallmalloc_sweep_keep(p)) {
            /* the location should be freed now */
            if (flprev == NULL) {
                flprev = (struct small_free_loc_s *)p;
                flprev->next = fl;
                page_free = flprev;
            }
            else {
                assert(flprev->next == fl);
                flprev->next = (struct small_free_loc_s *)p;
                flprev = (struct small_free_loc_s *)p;
                flprev->next = fl;
            }
            any_object_dying = true;
        }
        else {
            any_object_remaining = true;
        }
    }
    if (!any_object_remaining) {
        ((struct small_free_loc_s *)baseptr)->nextpage = free_uniform_pages;
        free_uniform_pages = (struct small_free_loc_s *)baseptr;
    }
    else if (!any_object_dying) {
        get_fpsz(baseptr)->sz = szword;
    }
    else {
        check_order_inside_small_page(page_free);
        page_free->nextpage = small_page_lists[szword];
        small_page_lists[szword] = page_free;
    }
}

void _stm_smallmalloc_sweep(void)
{
    long i, szword;
    for (szword = 2; szword < GC_N_SMALL_REQUESTS; szword++) {
        struct small_free_loc_s *page = small_page_lists[szword];
        struct small_free_loc_s *nextpage;
        small_page_lists[szword] = NULL;

        /* process the pages that the various segments are busy filling */
        for (i = 0; i < NB_SEGMENTS; i++) {
            struct stm_priv_segment_info_s *pseg = get_priv_segment(i);
            struct small_free_loc_s **fl =
                    &pseg->small_malloc_data.loc_free[szword];
            if (*fl != NULL) {
                /* the entry in full_pages_object_size[] should already be
                   szword.  We reset it to 0. */
                fpsz_t *fpsz = get_fpsz((char *)*fl);
                assert(fpsz->sz == szword);
                fpsz->sz = 0;
                sweep_small_page(getbaseptr(*fl), *fl, szword);
                *fl = NULL;
            }
        }

        /* process all the other partially-filled pages */
        while (page != NULL) {
            /* for every page in small_page_lists: assert that the
               corresponding full_pages_object_size[] entry is 0 */
            assert(get_fpsz((char *)page)->sz == 0);
            nextpage = page->nextpage;
            sweep_small_page(getbaseptr(page), page, szword);
            page = nextpage;
        }
    }

    /* process the really full pages, which are the ones which still
       have a non-zero full_pages_object_size[] entry */
    char *pageptr = uninitialized_page_stop;
    fpsz_t *fpsz_start = get_fpsz(pageptr);
    fpsz_t *fpsz_end = &full_pages_object_size[PAGE_SMSIZE_END -
                                               PAGE_SMSIZE_START];
    fpsz_t *fpsz;
    for (fpsz = fpsz_start; fpsz < fpsz_end; fpsz++, pageptr += 4096) {
        uint8_t sz = fpsz->sz;
        if (sz != 0) {
            fpsz->sz = 0;
            sweep_small_page(pageptr, NULL, sz);
        }
    }
}
