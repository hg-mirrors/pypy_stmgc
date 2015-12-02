#ifndef _STM_CORE_H_
# error "must be compiled via stmgc.c"
#endif

#include <fcntl.h>           /* For O_* constants */

/* XXX this is currently not doing copy-on-write, but simply forces a
   copy of all pages as soon as fork() is called. */


static char *fork_big_copy = NULL;
static int fork_big_copy_fd;
static stm_thread_local_t *fork_this_tl;
static bool fork_was_in_transaction;


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
    fprintf(stderr, "[forking: for now, this operation can take some time]\n");

    stm_thread_local_t *this_tl = NULL;
    stm_thread_local_t *tl = stm_all_thread_locals;
    do {
        if (pthread_equal(*_get_cpth(tl), pthread_self())) {
            if (this_tl != NULL)
                stm_fatalerror("fork(): found several stm_thread_local_t"
                               " from the same thread");
            this_tl = tl;
        }
        tl = tl->next;
    } while (tl != stm_all_thread_locals);

    if (this_tl == NULL)
        stm_fatalerror("fork(): found no stm_thread_local_t from this thread");
    s_mutex_unlock();

    bool was_in_transaction = _stm_in_transaction(this_tl);
    if (!was_in_transaction)
        _stm_start_transaction(this_tl);
    assert(in_transaction(this_tl));

    stm_become_inevitable(this_tl, "fork");
    /* Note that the line above can still fail and abort, which should
       be fine */

    s_mutex_lock();
    synchronize_all_threads(STOP_OTHERS_UNTIL_MUTEX_UNLOCK);


    /* make a new SHARED mmap for the child to use and copy everything
       in seg0 there. */
    char name[128];
    char *reason = "forking";
    sprintf(name, "/stmgc-c7-bigmem-%ld",
            (long)getpid());
    int big_copy_fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    shm_unlink(name);
    if (big_copy_fd == -1)
        stm_fatalerror("%s failed (stm_open): %m", reason);
    if (ftruncate(big_copy_fd, NB_SHARED_PAGES * 4096UL) != 0)
        stm_fatalerror("%s failed (ftruncate): %m", reason);
    char *big_copy = mmap(
        NULL,                     /* addr */
        NB_SHARED_PAGES * 4096UL, /* len */
        PROT_READ | PROT_WRITE,
        MAP_SHARED | MAP_NORESERVE,
        big_copy_fd, 0); /* file & offset */
    if (big_copy == MAP_FAILED)
        stm_fatalerror("%s failed (mmap): %m", reason);

    /* Copy all the data from the two ranges of objects (large, small)
       into the new mmap
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

        char *src = stm_object_pages + pagenum * 4096UL;
        char *dst = big_copy + pagenum * 4096UL - END_NURSERY_PAGE * 4096UL;
        pagecopy(dst, src);

        pagenum++;
    }

    assert(fork_big_copy == NULL);
    fork_big_copy = big_copy;
    fork_big_copy_fd = big_copy_fd;
    fork_this_tl = this_tl;
    fork_was_in_transaction = was_in_transaction;

    assert(_has_mutex());
    dprintf(("forksupport_prepare: from %p %p\n", fork_this_tl,
             fork_this_tl->creating_pthread[0]));
}

static void forksupport_parent(void)
{
    if (stm_object_pages == NULL)
        return;

    dprintf(("forksupport_parent: continuing to run %p %p\n", fork_this_tl,
             fork_this_tl->creating_pthread[0]));
    assert(_has_mutex());
    assert(_is_tl_registered(fork_this_tl));

    /* In the parent, after fork(), we can simply forget about the big copy
       that we made for the child.
    */
    assert(fork_big_copy != NULL);
    munmap(fork_big_copy, NB_SHARED_PAGES * 4096UL);
    fork_big_copy = NULL;
    close(fork_big_copy_fd);
    bool was_in_transaction = fork_was_in_transaction;
    s_mutex_unlock();

    if (!was_in_transaction) {
        _stm_commit_transaction();
    }

    dprintf(("forksupport_parent: continuing to run\n"));
}

static void fork_abort_thread(long i)
{
    struct stm_priv_segment_info_s *pr = get_priv_segment(i);
    stm_thread_local_t *tl = pr->pub.running_thread;
    dprintf(("forksupport_child: abort in seg%ld\n", i));
    assert(tl->last_associated_segment_num == i);
    assert(in_transaction(tl));
    assert(pr->transaction_state != TS_INEVITABLE);
    ensure_gs_register(i);
    assert(STM_SEGMENT->segment_num == i);

    /* XXXXXXXXXXXXXXXX what about finalizers, callbacks, etc? XXXXXXXXXXXXXXXXXX*/
    s_mutex_lock();
    if (pr->transaction_state == TS_NONE) {
        /* just committed, TS_NONE but still has running_thread */

        /* do _finish_transaction() */
        STM_PSEGMENT->safe_point = SP_NO_TRANSACTION;
        _verify_cards_cleared_in_all_lists(get_priv_segment(STM_SEGMENT->segment_num));
        list_clear(STM_PSEGMENT->objects_pointing_to_nursery);
        list_clear(STM_PSEGMENT->old_objects_with_cards_set);
        list_clear(STM_PSEGMENT->large_overflow_objects);
        timing_event(tl, STM_TRANSACTION_ABORT);

        s_mutex_unlock();
        return;
    }

#ifndef NDEBUG
    pr->running_pthread = pthread_self();
#endif
    tl->shadowstack = NULL;
    pr->shadowstack_at_start_of_transaction = NULL;
    stm_rewind_jmp_forget(tl);
    abort_with_mutex_no_longjmp();
    s_mutex_unlock();
}

static void forksupport_child(void)
{
    if (stm_object_pages == NULL)
        return;

    /* this new process contains no other thread, so we can
       just release these locks early */
    s_mutex_unlock();

    /* Move the big_copy-map over to seg0 mmap and free the old mapping */
    assert(fork_big_copy != NULL);
    assert(stm_object_pages != NULL);

    char *dst = stm_object_pages + END_NURSERY_PAGE * 4096UL;
    void *res = mremap(fork_big_copy, NB_SHARED_PAGES * 4096UL, /* old_addr, old_sz */
                       NB_SHARED_PAGES * 4096UL,       /* new_size */
                       MREMAP_MAYMOVE | MREMAP_FIXED, dst);
    if (res != dst)
        stm_fatalerror("after fork: mremap failed: %m");

    munmap(fork_big_copy, NB_SHARED_PAGES * 4096UL);
    fork_big_copy = NULL;
    close(stm_object_pages_fd);
    stm_object_pages_fd = fork_big_copy_fd;

    /* Unregister all other stm_thread_local_t, mostly as a way to free
       the memory used by the shadowstacks
     */
    while (stm_all_thread_locals->next != stm_all_thread_locals) {
        if (stm_all_thread_locals == fork_this_tl)
            stm_unregister_thread_local(stm_all_thread_locals->next);
        else
            stm_unregister_thread_local(stm_all_thread_locals);
    }
    assert(stm_all_thread_locals == fork_this_tl);



    /* Force the interruption of other running segments (seg0 never runs)
     */
    long i;
    for (i = 1; i < NB_SEGMENTS; i++) {
        struct stm_priv_segment_info_s *pr = get_priv_segment(i);
        if (pr->pub.running_thread != NULL &&
            pr->pub.running_thread != fork_this_tl) {
            fork_abort_thread(i);
        }
    }

    /* Restore a few things: the new pthread_self(), and the %gs
       register */
    int segnum = fork_this_tl->last_associated_segment_num;
    assert(1 <= segnum && segnum < NB_SEGMENTS);
    *_get_cpth(fork_this_tl) = pthread_self();
    ensure_gs_register(segnum);
    assert(STM_SEGMENT->segment_num == segnum);

    if (!fork_was_in_transaction) {
        _stm_commit_transaction();
    }

    /* Done */
    dprintf(("forksupport_child: running one thread now\n"));
}


static void setup_forksupport(void)
{
    static bool fork_support_ready = false;

    if (!fork_support_ready) {
        int res = pthread_atfork(forksupport_prepare, forksupport_parent,
                                 forksupport_child);
        if (res != 0)
            stm_fatalerror("pthread_atfork() failed: %d", res);
        fork_support_ready = true;
    }
}
