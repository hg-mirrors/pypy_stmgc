#ifndef _STM_CORE_H_
# error "must be compiled via stmgc.c"
#endif


/* XXX this is currently not doing copy-on-write, but simply forces a
   copy of all shared pages as soon as fork() is called. */


static char *fork_big_copy = NULL;

static char *setup_mmap(char *reason);            /* forward, in setup.c */
static void do_or_redo_setup_after_fork(void);    /* forward, in setup.c */
static void do_or_redo_teardown_after_fork(void); /* forward, in setup.c */


static void forksupport_prepare(void)
{
    if (stm_object_pages == NULL)
        return;

    /* This silently assumes that fork() is not called from transactions.
       It's hard to check though...
     */
    s_mutex_lock();

    synchronize_all_threads();

    mutex_pages_lock();

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
