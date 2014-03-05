#ifndef _STM_CORE_H_
# error "must be compiled via stmgc.c"
#endif


void stm_setup(void)
{
    /* Check that some values are acceptable */
    assert(NB_SEGMENTS <= NB_SEGMENTS_MAX);
    assert(4096 <= ((uintptr_t)STM_SEGMENT));
    assert((uintptr_t)STM_SEGMENT == (uintptr_t)STM_PSEGMENT);
    assert(((uintptr_t)STM_PSEGMENT) + sizeof(*STM_PSEGMENT) <= 8192);
    assert(2 <= FIRST_READMARKER_PAGE);
    assert(FIRST_READMARKER_PAGE * 4096UL <= READMARKER_START);
    assert(READMARKER_START < READMARKER_END);
    assert(READMARKER_END <= 4096UL * FIRST_OBJECT_PAGE);
    assert(FIRST_OBJECT_PAGE < NB_PAGES);
    assert((NB_PAGES * 4096UL) >> 8 <= (FIRST_OBJECT_PAGE * 4096UL) >> 4);
    assert((END_NURSERY_PAGE * 4096UL) >> 8 <=
           (FIRST_READMARKER_PAGE * 4096UL));
    assert(_STM_FAST_ALLOC <= NB_NURSERY_PAGES * 4096);

    stm_object_pages = mmap(NULL, TOTAL_MEMORY,
                            PROT_READ | PROT_WRITE,
                            MAP_PAGES_FLAGS, -1, 0);
    if (stm_object_pages == MAP_FAILED)
        stm_fatalerror("initial stm_object_pages mmap() failed: %m\n");

    long i;
    for (i = 0; i < NB_SEGMENTS; i++) {
        char *segment_base = get_segment_base(i);
#ifdef STM_TESTS
        stm_other_pages = segment_base;
#endif

        /* In each segment, the first page is where TLPREFIX'ed
           NULL accesses land.  We mprotect it so that accesses fail. */
        mprotect(segment_base, 4096, PROT_NONE);

        /* Fill the TLS page (page 1) with 0xDC, for debugging */
        memset(REAL_ADDRESS(segment_base, 4096), 0xDC, 4096);
        /* Make a "hole" at STM_PSEGMENT */
        memset(REAL_ADDRESS(segment_base, STM_PSEGMENT), 0,
               sizeof(*STM_PSEGMENT));

        /* Pages in range(2, FIRST_READMARKER_PAGE) are never used */
        if (FIRST_READMARKER_PAGE > 2)
            mprotect(segment_base + 8192,
                     (FIRST_READMARKER_PAGE - 2) * 4096UL,
                     PROT_NONE);

        struct stm_priv_segment_info_s *pr = get_priv_segment(i);
        assert(i + 1 < 255);   /* 255 is WL_VISITED in gcpage.c */
        pr->write_lock_num = i + 1;
        pr->pub.segment_num = i;
        pr->pub.segment_base = segment_base;
        pr->objects_pointing_to_nursery = NULL;
        pr->large_overflow_objects = NULL;
        pr->modified_old_objects = list_create();
        pr->young_outside_nursery = tree_create();
        pr->nursery_objects_shadows = tree_create();
        pr->overflow_number = GCFLAG_OVERFLOW_NUMBER_bit0 * (i + 1);
        highest_overflow_number = pr->overflow_number;
    }

    /* The pages are shared lazily, as remap_file_pages() takes a relatively
       long time for each page.

       The read markers are initially zero, which is correct:
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
    setup_pages();
}

void stm_teardown(void)
{
    /* This function is called during testing, but normal programs don't
       need to call it. */
    assert(!_has_mutex());

    long i;
    for (i = 0; i < NB_SEGMENTS; i++) {
        struct stm_priv_segment_info_s *pr = get_priv_segment(i);
        assert(pr->objects_pointing_to_nursery == NULL);
        assert(pr->large_overflow_objects == NULL);
        list_free(pr->modified_old_objects);
        tree_free(pr->young_outside_nursery);
        tree_free(pr->nursery_objects_shadows);
    }

    munmap(stm_object_pages, TOTAL_MEMORY);
    stm_object_pages = NULL;

    memset(flag_page_private, 0, sizeof(flag_page_private));

    teardown_core();
    teardown_sync();
    teardown_gcpage();
    teardown_nursery();
    teardown_pages();
}

void _init_shadow_stack(stm_thread_local_t *tl)
{
    object_t **s = (object_t **)malloc(SHADOW_STACK_SIZE * sizeof(object_t *));
    assert(s);
    tl->shadowstack = s;
    tl->shadowstack_base = s;
}

void _done_shadow_stack(stm_thread_local_t *tl)
{
    free(tl->shadowstack_base);
    tl->shadowstack = NULL;
    tl->shadowstack_base = NULL;
}


void stm_register_thread_local(stm_thread_local_t *tl)
{
    int num;
    s_mutex_lock();
    if (stm_all_thread_locals == NULL) {
        stm_all_thread_locals = tl->next = tl->prev = tl;
        num = 0;
    }
    else {
        tl->next = stm_all_thread_locals;
        tl->prev = stm_all_thread_locals->prev;
        stm_all_thread_locals->prev->next = tl;
        stm_all_thread_locals->prev = tl;
        num = tl->prev->associated_segment_num + 1;
    }
    tl->thread_local_obj = NULL;

    /* assign numbers consecutively, but that's for tests; we could also
       assign the same number to all of them and they would get their own
       numbers automatically. */
    num = num % NB_SEGMENTS;
    tl->associated_segment_num = num;
    _init_shadow_stack(tl);
    set_gs_register(get_segment_base(num));
    s_mutex_unlock();
}

void stm_unregister_thread_local(stm_thread_local_t *tl)
{
    s_mutex_lock();
    assert(tl->next != NULL);
    _done_shadow_stack(tl);
    if (tl == stm_all_thread_locals) {
        stm_all_thread_locals = stm_all_thread_locals->next;
        if (tl == stm_all_thread_locals) {
            stm_all_thread_locals = NULL;
            s_mutex_unlock();
            return;
        }
    }
    tl->prev->next = tl->next;
    tl->next->prev = tl->prev;
    tl->prev = NULL;
    tl->next = NULL;
    s_mutex_unlock();
}

__attribute__((unused))
static bool _is_tl_registered(stm_thread_local_t *tl)
{
    return tl->next != NULL;
}
