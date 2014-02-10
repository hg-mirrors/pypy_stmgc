#ifndef _STM_CORE_H_
# error "must be compiled via stmgc.c"
#endif


void stm_setup(void)
{
#if 0
    _stm_reset_shared_lock();
    _stm_reset_pages();

    inevitable_lock = 0;
#endif

    /* Check that some values are acceptable */
    assert(4096 <= ((uintptr_t)STM_REGION));
    assert((uintptr_t)STM_REGION == (uintptr_t)STM_PREGION);
    assert(((uintptr_t)STM_PREGION) + sizeof(*STM_PREGION) <= 8192);
    assert(2 <= FIRST_READMARKER_PAGE);
    assert(FIRST_READMARKER_PAGE * 4096UL <= READMARKER_START);
    assert(READMARKER_START < READMARKER_END);
    assert(READMARKER_END <= 4096UL * FIRST_OBJECT_PAGE);
    assert(FIRST_OBJECT_PAGE < NB_PAGES);

    stm_object_pages = mmap(NULL, TOTAL_MEMORY,
                            PROT_READ | PROT_WRITE,
                            MAP_PAGES_FLAGS, -1, 0);
    if (stm_object_pages == MAP_FAILED) {
        perror("stm_object_pages mmap");
        abort();
    }

    long i;
    for (i = 0; i < NB_REGIONS; i++) {
        char *region_base = get_region_base(i);

        /* In each region, the first page is where TLPREFIX'ed
           NULL accesses land.  We mprotect it so that accesses fail. */
        mprotect(region_base, 4096, PROT_NONE);

        /* Fill the TLS page (page 1) with 0xDD, for debugging */
        memset(REAL_ADDRESS(region_base, 4096), 0xDD, 4096);
        /* Make a "hole" at STM_PREGION */
        memset(REAL_ADDRESS(region_base, STM_PREGION), 0,
               sizeof(*STM_PREGION));

        /* Pages in range(2, FIRST_READMARKER_PAGE) are never used */
        if (FIRST_READMARKER_PAGE > 2)
            mprotect(region_base + 8192, (FIRST_READMARKER_PAGE - 2) * 4096UL,
                     PROT_NONE);

        struct stm_priv_region_info_s *pr = get_priv_region(i);
        pr->pub.region_num = i;
        pr->pub.region_base = region_base;
    }

    /* Make the nursery pages shared.  The other pages are
       shared lazily, as remap_file_pages() takes a relatively
       long time for each page. */
    pages_initialize_shared(FIRST_NURSERY_PAGE, NB_NURSERY_PAGES);

#if 0
    stm_largemalloc_init(heap, HEAP_PAGES * 4096UL);
#endif
}

void stm_teardown(void)
{
    /* This function is called during testing, but normal programs don't
       need to call it. */
    munmap(stm_object_pages, TOTAL_MEMORY);
    stm_object_pages = NULL;

    memset(flag_page_private, 0, sizeof(flag_page_private));
}

void stm_register_thread_local(stm_thread_local_t *tl)
{
    if (stm_thread_locals == NULL) {
        stm_thread_locals = tl->next = tl->prev = tl;
    }
    else {
        tl->next = stm_thread_locals;
        tl->prev = stm_thread_locals->prev;
        stm_thread_locals->prev->next = tl;
        stm_thread_locals->prev = tl;
    }
    tl->associated_region = get_region(0);
}

void stm_unregister_thread_local(stm_thread_local_t *tl)
{
    if (tl == stm_thread_locals) {
        stm_thread_locals = stm_thread_locals->next;
        if (tl == stm_thread_locals) {
            stm_thread_locals = NULL;
            return;
        }
    }
    tl->prev->next = tl->next;
    tl->next->prev = tl->prev;
}
