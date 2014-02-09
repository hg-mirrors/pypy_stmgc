

void stm_setup(void)
{
#if 0
    _stm_reset_shared_lock();
    _stm_reset_pages();

    inevitable_lock = 0;
    
    /* Check that some values are acceptable */
    assert(4096 <= ((uintptr_t)_STM_TL));
    assert(((uintptr_t)_STM_TL) == ((uintptr_t)_STM_TL));
    assert(((uintptr_t)_STM_TL) + sizeof(*_STM_TL) <= 8192);
    assert(2 <= FIRST_READMARKER_PAGE);
    assert(FIRST_READMARKER_PAGE * 4096UL <= READMARKER_START);
    assert(READMARKER_START < READMARKER_END);
    assert(READMARKER_END <= 4096UL * FIRST_OBJECT_PAGE);
    assert(FIRST_OBJECT_PAGE < NB_PAGES);
    assert((NB_NURSERY_PAGES * 4096) % NURSERY_SECTION == 0);

    object_pages = mmap(NULL, TOTAL_MEMORY,
                        PROT_READ | PROT_WRITE,
                        MAP_PAGES_FLAGS, -1, 0);
    if (object_pages == MAP_FAILED) {
        perror("object_pages mmap");
        abort();
    }

    long i;
    for (i = 0; i < NB_THREADS; i++) {
        char *thread_base = get_thread_base(i);

        /* In each thread's section, the first page is where TLPREFIX'ed
           NULL accesses land.  We mprotect it so that accesses fail. */
        mprotect(thread_base, 4096, PROT_NONE);

        /* Fill the TLS page (page 1) with 0xDD */
        memset(REAL_ADDRESS(thread_base, 4096), 0xDD, 4096);
        /* Make a "hole" at _STM_TL / _STM_TL */
        memset(REAL_ADDRESS(thread_base, _STM_TL), 0, sizeof(*_STM_TL));

        /* Pages in range(2, FIRST_READMARKER_PAGE) are never used */
        if (FIRST_READMARKER_PAGE > 2)
            mprotect(thread_base + 8192, (FIRST_READMARKER_PAGE - 2) * 4096UL,
                         PROT_NONE);

        struct _thread_local1_s *th =
            (struct _thread_local1_s *)REAL_ADDRESS(thread_base, _STM_TL);

        th->thread_num = i;
        th->thread_base = thread_base;

        if (i > 0) {
            int res;
            res = remap_file_pages(
                    thread_base + FIRST_AFTER_NURSERY_PAGE * 4096UL,
                    (NB_PAGES - FIRST_AFTER_NURSERY_PAGE) * 4096UL,
                    0, FIRST_AFTER_NURSERY_PAGE, 0);

            if (res != 0) {
                perror("remap_file_pages");
                abort();
            }
        }
    }

    for (i = FIRST_NURSERY_PAGE; i < FIRST_AFTER_NURSERY_PAGE; i++)
        stm_set_page_flag(i, PRIVATE_PAGE); /* nursery is private.
                                                or should it be UNCOMMITTED??? */
    
    num_threads_started = 0;

    assert(HEAP_PAGES < NB_PAGES - FIRST_AFTER_NURSERY_PAGE);
    assert(HEAP_PAGES > 10);

    uintptr_t first_heap = stm_pages_reserve(HEAP_PAGES);
    char *heap = REAL_ADDRESS(get_thread_base(0), first_heap * 4096UL); 
    assert(memset(heap, 0xcd, HEAP_PAGES * 4096)); // testing
    stm_largemalloc_init(heap, HEAP_PAGES * 4096UL);

    for (i = 0; i < NB_THREADS; i++) {
        _stm_setup_static_thread();
    }
#endif
}
