#ifndef _STM_CORE_H_
# error "must be compiled via stmgc.c"
#endif

#include <signal.h>
#include <fcntl.h>           /* For O_* constants */

static void setup_mmap(char *reason)
{
    /* reserve the whole virtual memory space of the program for
       all segments: (for now in one big block, but later could be
       allocated per-segment) */
    stm_object_pages = mmap(NULL, TOTAL_MEMORY, PROT_NONE,
                            MAP_PRIVATE | MAP_NORESERVE | MAP_ANONYMOUS,
                            -1, 0);
    if (stm_object_pages == MAP_FAILED)
        stm_fatalerror("%s failed (mmap): %m", reason);
}

static void setup_protection_settings(void)
{
    long i;
    for (i = 0; i < NB_SEGMENTS; i++) {
        char *segment_base = get_segment_base(i);

        /* In each segment, the second page is where STM_SEGMENT lands. */
        mprotect(segment_base + 4096, 4096, PROT_READ | PROT_WRITE);

        /* Make the read marker pages accessible, as well as the nursery. */
        mprotect(segment_base + FIRST_READMARKER_PAGE * 4096,
                 (NB_READMARKER_PAGES + NB_NURSERY_PAGES) * 4096,
                 PROT_READ | PROT_WRITE);
    }

    /* make the sharing segment writable for the memory allocator: */
    mprotect(stm_object_pages + END_NURSERY_PAGE * 4096UL,
             (NB_PAGES - END_NURSERY_PAGE) * 4096UL,
             PROT_READ | PROT_WRITE);
}


static void setup_signal_handler(void)
{
    struct sigaction act;
    memset(&act, 0, sizeof(act));

	act.sa_sigaction = &_signal_handler;
	/* The SA_SIGINFO flag tells sigaction() to use the sa_sigaction field, not sa_handler. */
	act.sa_flags = SA_SIGINFO | SA_NODEFER;

	if (sigaction(SIGSEGV, &act, NULL) < 0) {
		perror ("sigaction");
		abort();
	}
}

void stm_setup(void)
{
    /* Check that some values are acceptable */
    assert(4096 <= ((uintptr_t)STM_SEGMENT));
    assert((uintptr_t)STM_SEGMENT == (uintptr_t)STM_PSEGMENT);
    assert(((uintptr_t)STM_PSEGMENT) + sizeof(*STM_PSEGMENT) <= FIRST_READMARKER_PAGE*4096);

    assert(NB_SEGMENTS <= NB_SEGMENTS_MAX);
    assert(FIRST_READMARKER_PAGE * 4096UL <= READMARKER_START);
    assert(READMARKER_START < READMARKER_END);
    assert(READMARKER_END <= 4096UL * FIRST_OBJECT_PAGE);
    assert(FIRST_OBJECT_PAGE < NB_PAGES);
    assert((NB_PAGES * 4096UL) >> 8 <= (FIRST_OBJECT_PAGE * 4096UL) >> 4);
    assert((END_NURSERY_PAGE * 4096UL) >> 8 <=
           (FIRST_READMARKER_PAGE * 4096UL));
    assert(_STM_FAST_ALLOC <= NB_NURSERY_PAGES * 4096);

    setup_mmap("initial stm_object_pages mmap()");

    assert(stm_object_pages);

    setup_protection_settings();
    setup_signal_handler();

    commit_log_root.next = NULL;
    commit_log_root.segment_num = -1;
    commit_log_root.rev_num = 0;
    commit_log_root.written_count = 0;

    long i;
    /* including seg0 */
    for (i = 0; i < NB_SEGMENTS; i++) {
        char *segment_base = get_segment_base(i);

        /* Fill the TLS page (page 1) with 0xDC, for debugging */
        memset(REAL_ADDRESS(segment_base, ((uintptr_t)STM_PSEGMENT/4096) * 4096), 0xDC, 4096);
        /* Make a "hole" at STM_PSEGMENT (which includes STM_SEGMENT) */
        memset(REAL_ADDRESS(segment_base, STM_PSEGMENT), 0,
               sizeof(*STM_PSEGMENT));

        /* Initialize STM_PSEGMENT */
        struct stm_priv_segment_info_s *pr = get_priv_segment(i);
        assert(0 <= i && i < 255);   /* 255 is WL_VISITED in gcpage.c */
        pr->pub.segment_num = i;
        pr->pub.segment_base = segment_base;
        pr->modified_old_objects = list_create();
        pr->new_objects = list_create();
        pr->young_weakrefs = list_create();
        pr->old_weakrefs = list_create();
        pr->objects_pointing_to_nursery = list_create();
        pr->young_outside_nursery = tree_create();
        pr->nursery_objects_shadows = tree_create();
        pr->callbacks_on_commit_and_abort[0] = tree_create();
        pr->callbacks_on_commit_and_abort[1] = tree_create();
        pr->young_objects_with_light_finalizers = list_create();
        pr->old_objects_with_light_finalizers = list_create();

        pr->last_commit_log_entry = &commit_log_root;
        pr->pub.transaction_read_version = 0xff;
    }

    /* The pages are shared lazily, as remap_file_pages() takes a relatively
       long time for each page.

       The read markers are initially zero, but we set anyway
       transaction_read_version to 0xff in order to force the first
       transaction to "clear" the read markers by mapping a different,
       private range of addresses.
    */

    setup_sync();
    setup_nursery();
    setup_gcpage();
    setup_pages();
    setup_forksupport();
    setup_finalizer();

    set_gs_register(get_segment_base(0));
}

void stm_teardown(void)
{
    /* This function is called during testing, but normal programs don't
       need to call it. */
    assert(!_has_mutex());

    assert(commit_log_root.segment_num == -1);

    long i;
    for (i = 0; i < NB_SEGMENTS; i++) {
        struct stm_priv_segment_info_s *pr = get_priv_segment(i);
        assert(list_is_empty(pr->objects_pointing_to_nursery));
        list_free(pr->objects_pointing_to_nursery);
        list_free(pr->modified_old_objects);
        assert(list_is_empty(pr->new_objects));
        list_free(pr->new_objects);
        list_free(pr->young_weakrefs);
        list_free(pr->old_weakrefs);
        tree_free(pr->young_outside_nursery);
        tree_free(pr->nursery_objects_shadows);
        tree_free(pr->callbacks_on_commit_and_abort[0]);
        tree_free(pr->callbacks_on_commit_and_abort[1]);
        list_free(pr->young_objects_with_light_finalizers);
        list_free(pr->old_objects_with_light_finalizers);
    }

    munmap(stm_object_pages, TOTAL_MEMORY);
    stm_object_pages = NULL;
    commit_log_root.next = NULL; /* xxx:free them */
    commit_log_root.segment_num = -1;

    teardown_finalizer();
    teardown_sync();
    teardown_gcpage();
    teardown_smallmalloc();
    teardown_pages();
}

static void _shadowstack_trap_page(char *start, int prot)
{
    size_t bsize = STM_SHADOW_STACK_DEPTH * sizeof(struct stm_shadowentry_s);
    char *end = start + bsize + 4095;
    end -= (((uintptr_t)end) & 4095);
    mprotect(end, 4096, prot);
}

static void _init_shadow_stack(stm_thread_local_t *tl)
{
    size_t bsize = STM_SHADOW_STACK_DEPTH * sizeof(struct stm_shadowentry_s);
    char *start = malloc(bsize + 8192);  /* for the trap page, plus rounding */
    if (!start)
        stm_fatalerror("can't allocate shadow stack");

    /* set up a trap page: if the shadowstack overflows, it will
       crash in a clean segfault */
    _shadowstack_trap_page(start, PROT_NONE);

    struct stm_shadowentry_s *s = (struct stm_shadowentry_s *)start;
    tl->shadowstack = s;
    tl->shadowstack_base = s;
    STM_PUSH_ROOT(*tl, -1);
}

static void _done_shadow_stack(stm_thread_local_t *tl)
{
    assert(tl->shadowstack > tl->shadowstack_base);
    assert(tl->shadowstack_base->ss == (object_t *)-1);

    char *start = (char *)tl->shadowstack_base;
    _shadowstack_trap_page(start, PROT_READ | PROT_WRITE);

    free(tl->shadowstack_base);
    tl->shadowstack = NULL;
    tl->shadowstack_base = NULL;
}


static pthread_t *_get_cpth(stm_thread_local_t *tl)
{
    assert(sizeof(pthread_t) <= sizeof(tl->creating_pthread));
    return (pthread_t *)(tl->creating_pthread);
}

void stm_register_thread_local(stm_thread_local_t *tl)
{
    int num;
    s_mutex_lock();
    if (stm_all_thread_locals == NULL) {
        stm_all_thread_locals = tl->next = tl->prev = tl;
        num = 0;
    } else {
        tl->next = stm_all_thread_locals;
        tl->prev = stm_all_thread_locals->prev;
        stm_all_thread_locals->prev->next = tl;
        stm_all_thread_locals->prev = tl;
        num = (tl->prev->last_associated_segment_num) % (NB_SEGMENTS-1);
    }
    tl->thread_local_obj = NULL;

    /* assign numbers consecutively, but that's for tests; we could also
       assign the same number to all of them and they would get their own
       numbers automatically. */
    tl->associated_segment_num = -1;
    tl->last_associated_segment_num = num + 1;
    *_get_cpth(tl) = pthread_self();
    _init_shadow_stack(tl);
    set_gs_register(get_segment_base(num + 1));
    s_mutex_unlock();

    DEBUG_EXPECT_SEGFAULT(true);

    if (num == 0) {
        dprintf(("STM_GC_NURSERY: %d\n", STM_GC_NURSERY));
        dprintf(("NB_PAGES: %d\n", NB_PAGES));
        dprintf(("NB_SEGMENTS: %d\n", NB_SEGMENTS));
        dprintf(("FIRST_OBJECT_PAGE=FIRST_NURSERY_PAGE: %lu\n", FIRST_OBJECT_PAGE));
        dprintf(("END_NURSERY_PAGE: %lu\n", END_NURSERY_PAGE));
        dprintf(("NB_SHARED_PAGES: %lu\n", NB_SHARED_PAGES));
    }
}

void stm_unregister_thread_local(stm_thread_local_t *tl)
{
    s_mutex_lock();
    assert(tl->prev != NULL);
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
