#ifndef _STM_CORE_H_
# error "must be compiled via stmgc.c"
#endif


/* XXX this is currently not doing copy-on-write, but simply forces a
   copy of all shared pages as soon as fork() is called. */


static char *fork_big_copy = NULL;
static stm_thread_local_t *fork_this_tl;
static bool fork_was_in_transaction;

static char *setup_mmap(char *reason);            /* forward, in setup.c */
static void do_or_redo_setup_after_fork(void);    /* forward, in setup.c */
static void do_or_redo_teardown_after_fork(void); /* forward, in setup.c */
static pthread_t *_get_cpth(stm_thread_local_t *);/* forward, in setup.c */


static void forksupport_prepare(void)
{
    if (stm_object_pages == NULL)
        return;

    /* So far we attempt to check this by walking all stm_thread_local_t,
       marking the one from the current thread, and verifying that it's not
       running a transaction.  This assumes that the stm_thread_local_t is just
       a __thread variable, so never changes threads.
    */
    s_mutex_lock();

    dprintf(("forksupport_prepare\n"));

    fork_this_tl = NULL;
    stm_thread_local_t *tl = stm_all_thread_locals;
    do {
        if (pthread_equal(*_get_cpth(tl), pthread_self())) {
            if (fork_this_tl != NULL)
                stm_fatalerror("fork(): found several stm_thread_local_t"
                               " from the same thread");
            fork_this_tl = tl;
        }
        tl = tl->next;
    } while (tl != stm_all_thread_locals);

    if (fork_this_tl == NULL)
        stm_fatalerror("fork(): found no stm_thread_local_t from this thread");
    s_mutex_unlock();

    /* Run a commit without releasing the mutex at the end; if necessary,
       actually start a dummy inevitable transaction for this
    */
    fork_was_in_transaction = _stm_in_transaction(fork_this_tl);
    if (!fork_was_in_transaction)
        stm_start_inevitable_transaction(fork_this_tl);
    _stm_commit_transaction(/*keep_the_lock_at_the_end =*/ 1);

    printf("fork_was_in_transaction: %d\n"
           "fork_this_tl->associated_segment_num: %d\n",
           (int)fork_was_in_transaction,
           (int)fork_this_tl->associated_segment_num);

    /* Note that the commit can still fail and abort, which should be fine */

    mutex_pages_lock();

    /* Make a new mmap at some other address, but of the same size as
       the standard mmap at stm_object_pages
    */
    char *big_copy = setup_mmap("stmgc's fork support");

    /* Copy each of the segment infos into the new mmap
     */
    long i;
    for (i = 1; i <= NB_SEGMENTS; i++) {
        struct stm_priv_segment_info_s *src = get_priv_segment(i);
        char *dst = big_copy + (((char *)src) - stm_object_pages);
        *(struct stm_priv_segment_info_s *)dst = *src;
    }

    /* Copy all the data from the two ranges of objects (large, small)
       into the new mmap --- but only the shared objects
    */
    uintptr_t pagenum, endpagenum;
    pagenum = END_NURSERY_PAGE;   /* starts after the nursery */
    endpagenum = (uninitialized_page_start - stm_object_pages) / 4096UL;
    if (endpagenum < NB_PAGES)
        endpagenum++;   /* the next page too, because it might contain
                           data from largemalloc */

    while (1) {
        if (UNLIKELY(pagenum == endpagenum)) {
            /* we reach this point usually twice, because there are
               more pages after 'uninitialized_page_stop' */
            if (endpagenum == NB_PAGES)
                break;   /* done */
            pagenum = (uninitialized_page_stop - stm_object_pages) / 4096UL;
            pagenum--;   /* the prev page too, because it does contain
                            data from largemalloc */
            endpagenum = NB_PAGES;
        }

        pagecopy(big_copy + pagenum * 4096UL,
                 stm_object_pages + pagenum * 4096UL);
        pagenum++;
    }

    assert(fork_big_copy == NULL);
    fork_big_copy = big_copy;

    assert(_has_mutex());
}

static void forksupport_parent(void)
{
    if (stm_object_pages == NULL)
        return;

    assert(_is_tl_registered(fork_this_tl));
    assert(_has_mutex());

    /* In the parent, after fork(), we can simply forget about the big copy
       that we made for the child.
    */
    assert(fork_big_copy != NULL);
    munmap(fork_big_copy, TOTAL_MEMORY);
    fork_big_copy = NULL;

    dprintf(("forksupport_parent: continuing to run\n"));

    mutex_pages_unlock();

    printf("AFTER: fork_was_in_transaction: %d\n"
           "fork_this_tl->associated_segment_num: %d\n",
           (int)fork_was_in_transaction,
           (int)fork_this_tl->associated_segment_num);

    if (fork_was_in_transaction) {
        _stm_start_transaction(fork_this_tl, NULL,
                               /*already_got_the_lock =*/ 1);
    }
    else {
        s_mutex_unlock();
    }
}

static void forksupport_child(void)
{
    if (stm_object_pages == NULL)
        return;
    abort();

    /* this new process contains no other thread, so we can
       just release these locks early */
    mutex_pages_unlock();
    s_mutex_unlock();

    /* Move the copy of the mmap over the old one, overwriting it
       and thus freeing the old mapping in this process
    */
    assert(fork_big_copy != NULL);
    assert(stm_object_pages != NULL);
    void *res = mremap(fork_big_copy, TOTAL_MEMORY, TOTAL_MEMORY,
                       MREMAP_MAYMOVE | MREMAP_FIXED,
                       stm_object_pages);
    if (res != stm_object_pages)
        stm_fatalerror("after fork: mremap failed: %m");
    fork_big_copy = NULL;

    /* Unregister all other stm_thread_local_t, mostly as a way to free
       the memory used by the shadowstacks
     */
    assert(fork_this_tl != NULL);
    while (stm_all_thread_locals->next != stm_all_thread_locals) {
        if (stm_all_thread_locals == fork_this_tl)
            stm_unregister_thread_local(stm_all_thread_locals->next);
        else
            stm_unregister_thread_local(stm_all_thread_locals);
    }
    assert(stm_all_thread_locals == fork_this_tl);

    /* Restore a few things: the new pthread_self(), and the %gs
       register (although I suppose it should be preserved by fork())
    */
    *_get_cpth(fork_this_tl) = pthread_self();
    set_gs_register(get_segment_base(fork_this_tl->associated_segment_num));

    /* Call a subset of stm_teardown() / stm_setup() to free and
       recreate the necessary data in all segments, and to clean up some
       of the global data like the big arrays that don't make sense any
       more.  We keep other things like the smallmalloc and largemalloc
       internal state.
    */
    do_or_redo_teardown_after_fork();
    do_or_redo_setup_after_fork();

    /* Make all pages shared again.
     */
    mutex_pages_lock();
    uintptr_t start = END_NURSERY_PAGE;
    uintptr_t stop  = (uninitialized_page_start - stm_object_pages) / 4096UL;
    pages_initialize_shared(start, stop - start);
    start = (uninitialized_page_stop - stm_object_pages) / 4096UL;
    stop = NB_PAGES;
    pages_initialize_shared(start, stop - start);
    mutex_pages_unlock();

    /* Now restart the transaction if needed
     */
    if (fork_was_in_transaction)
        stm_start_inevitable_transaction(fork_this_tl);

    dprintf(("forksupport_child: running one thread now\n"));
}


static void setup_forksupport(void)
{
    static bool fork_support_ready = false;

    if (!fork_support_ready) {
        int res = pthread_atfork(forksupport_prepare, forksupport_parent,
                                 forksupport_child);
        if (res != 0)
            stm_fatalerror("pthread_atfork() failed: %m");
        fork_support_ready = true;
    }
}
