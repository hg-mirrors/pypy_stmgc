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
    assert(4096 <= ((uintptr_t)STM_SEGMENT));
    assert((uintptr_t)STM_SEGMENT == (uintptr_t)STM_PSEGMENT);
    assert(((uintptr_t)STM_PSEGMENT) + sizeof(*STM_PSEGMENT) <= 8192);
    assert(2 <= FIRST_READMARKER_PAGE);
    assert(FIRST_READMARKER_PAGE * 4096UL <= READMARKER_START);
    assert(READMARKER_START < READMARKER_END);
    assert(READMARKER_END <= 4096UL * FIRST_OBJECT_PAGE);
    assert(FIRST_OBJECT_PAGE < NB_PAGES);
    assert(CREATMARKER_START >= 8192);
    assert(2 <= FIRST_CREATMARKER_PAGE);
    assert(FIRST_CREATMARKER_PAGE <= FIRST_READMARKER_PAGE);
    assert((NB_PAGES * 4096UL) >> 8 <= (FIRST_OBJECT_PAGE * 4096UL) >> 4);
    assert((END_NURSERY_PAGE * 4096UL) >> 8 <=
           (FIRST_READMARKER_PAGE * 4096UL));

    stm_object_pages = mmap(NULL, TOTAL_MEMORY,
                            PROT_READ | PROT_WRITE,
                            MAP_PAGES_FLAGS, -1, 0);
    if (stm_object_pages == MAP_FAILED) {
        perror("stm_object_pages mmap");
        abort();
    }

    long i;
    for (i = 0; i < NB_SEGMENTS; i++) {
        char *segment_base = get_segment_base(i);
#ifdef STM_TESTS
        stm_other_pages = segment_base;
#endif

        /* In each segment, the first page is where TLPREFIX'ed
           NULL accesses land.  We mprotect it so that accesses fail. */
        mprotect(segment_base, 4096, PROT_NONE);

        /* Fill the TLS page (page 1) with 0xDD, for debugging */
        memset(REAL_ADDRESS(segment_base, 4096), 0xDD, 4096);
        /* Make a "hole" at STM_PSEGMENT */
        memset(REAL_ADDRESS(segment_base, STM_PSEGMENT), 0,
               sizeof(*STM_PSEGMENT));

        /* Pages in range(2, FIRST_CREATMARKER_PAGE) are never used */
        if (FIRST_CREATMARKER_PAGE > 2)
            mprotect(segment_base + 8192,
                     (FIRST_CREATMARKER_PAGE - 2) * 4096UL,
                     PROT_NONE);

        struct stm_priv_segment_info_s *pr = get_priv_segment(i);
        assert(i + 1 < 256);
        pr->write_lock_num = i + 1;
        pr->pub.segment_num = i;
        pr->pub.segment_base = segment_base;
        pr->old_objects_to_trace = list_create();
        pr->modified_objects = list_create();
        pr->creation_markers = list_create();
    }

    /* Make the nursery pages shared.  The other pages are
       shared lazily, as remap_file_pages() takes a relatively
       long time for each page. */
    pages_initialize_shared(FIRST_NURSERY_PAGE, NB_NURSERY_PAGES);

    /* The read markers are initially zero, which is correct:
       STM_SEGMENT->transaction_read_version never contains zero,
       so a null read marker means "not read" whatever the
       current transaction_read_version is.

       The creation markers are initially zero, which is correct:
       it means "objects of this line of 256 bytes have not been
       allocated by the current transaction."
    */

    setup_sync();
    setup_nursery();
    setup_gcpage();
}

void stm_teardown(void)
{
    /* This function is called during testing, but normal programs don't
       need to call it. */
    long i;
    for (i = 0; i < NB_SEGMENTS; i++) {
        struct stm_priv_segment_info_s *pr = get_priv_segment(i);
        list_free(pr->old_objects_to_trace);
    }

    munmap(stm_object_pages, TOTAL_MEMORY);
    stm_object_pages = NULL;

    memset(flag_page_private, 0, sizeof(flag_page_private));

    teardown_core();
    teardown_sync();
    teardown_gcpage();
}

void stm_register_thread_local(stm_thread_local_t *tl)
{
    int num;
    if (stm_thread_locals == NULL) {
        stm_thread_locals = tl->next = tl->prev = tl;
        num = 0;
    }
    else {
        tl->next = stm_thread_locals;
        tl->prev = stm_thread_locals->prev;
        stm_thread_locals->prev->next = tl;
        stm_thread_locals->prev = tl;
        num = tl->prev->associated_segment_num + 1;
    }

    /* assign numbers consecutively, but that's for tests; we could also
       assign the same number to all of them and they would get their own
       numbers automatically. */
    num = num % NB_SEGMENTS;
    tl->associated_segment_num = num;
    set_gs_register(get_segment_base(num));
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
    tl->prev = NULL;
    tl->next = NULL;
}

static bool _is_tl_registered(stm_thread_local_t *tl) __attribute__((unused));
static bool _is_tl_registered(stm_thread_local_t *tl)
{
    return tl->next != NULL;
}
