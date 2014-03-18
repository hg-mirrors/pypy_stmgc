#ifndef _STM_CORE_H_
# error "must be compiled via stmgc.c"
#endif


/* XXX this is currently not doing copy-on-write, but simply forces a
   copy of all shared pages as soon as fork() is called. */


static char *fork_big_copy = NULL;
static stm_thread_local_t *fork_this_tl;

static char *setup_mmap(char *reason);            /* forward, in setup.c */
static void do_or_redo_setup_after_fork(void);    /* forward, in setup.c */
static void do_or_redo_teardown_after_fork(void); /* forward, in setup.c */
static pthread_t *_get_cpth(stm_thread_local_t *);/* forward, in setup.c */


static void forksupport_prepare(void)
{
    if (stm_object_pages == NULL)
        return;

    /* This assumes that fork() is not called from transactions.
       So far we attempt to check this by walking all stm_thread_local_t,
       marking the one from the current thread, and verifying that it's not
       running a transaction.  This assumes that the stm_thread_local_t is just
       a __thread variable, so never changes threads.
    */
    s_mutex_lock();

    synchronize_all_threads();

    mutex_pages_lock();

    fork_this_tl = NULL;
    stm_thread_local_t *tl = stm_all_thread_locals;
    do {
        if (pthread_equal(*_get_cpth(tl), pthread_self())) {
            if (_stm_in_transaction(tl))
                stm_fatalerror("fork(): cannot be used inside a transaction");
            if (fork_this_tl != NULL)
                stm_fatalerror("fork(): found several stm_thread_local_t"
                               " from the same thread");
            fork_this_tl = tl;
        }
        tl = tl->next;
    } while (tl != stm_all_thread_locals);

    if (fork_this_tl == NULL)
        stm_fatalerror("fork(): found no stm_thread_local_t from this thread");

    char *big_copy = setup_mmap("stmgc's fork support");

    uintptr_t pagenum, endpagenum;
    pagenum = END_NURSERY_PAGE;   /* starts after the nursery */
    endpagenum = (uninitialized_page_start - stm_object_pages) / 4096UL;

    while (1) {
        if (UNLIKELY(pagenum == endpagenum)) {
            /* we reach this point usually twice, because there are
               more pages after 'uninitialized_page_stop' */
            if (endpagenum == NB_PAGES)
                break;   /* done */
            pagenum = (uninitialized_page_stop - stm_object_pages) / 4096UL;
            endpagenum = NB_PAGES;
            if (pagenum == endpagenum)
                break;   /* no pages in the 2nd section, so done too */
        }

        pagecopy(big_copy + pagenum * 4096UL,
                 stm_object_pages + pagenum * 4096UL);
        pagenum++;
    }

    assert(fork_big_copy == NULL);
    fork_big_copy = big_copy;
}

static void forksupport_parent(void)
{
    if (stm_object_pages == NULL)
        return;

    assert(fork_big_copy != NULL);
    munmap(fork_big_copy, TOTAL_MEMORY);
    fork_big_copy = NULL;

    mutex_pages_unlock();
    s_mutex_unlock();
}

static void forksupport_child(void)
{
    if (stm_object_pages == NULL)
        return;

    /* xxx the stm_thread_local_t belonging to other threads just leak.
       Note that stm_all_thread_locals is preserved across a
       stm_teardown/stm_setup sequence. */

    mutex_pages_unlock();
    s_mutex_unlock();

    stm_thread_local_t *tl = stm_all_thread_locals;
    do {
        stm_thread_local_t *nexttl = tl->next;
        if (tl != fork_this_tl) {
            stm_unregister_thread_local(tl);
        }
        tl = nexttl;
    } while (tl != stm_all_thread_locals);

    do_or_redo_teardown_after_fork();

    assert(fork_big_copy != NULL);
    assert(stm_object_pages != NULL);
    mremap(fork_big_copy, TOTAL_MEMORY, TOTAL_MEMORY,
           MREMAP_MAYMOVE | MREMAP_FIXED,
           stm_object_pages);
    fork_big_copy = NULL;

    do_or_redo_setup_after_fork();
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
