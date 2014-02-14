#ifndef _STM_CORE_H_
# error "must be compiled via stmgc.c"
#endif


static uint8_t write_locks[READMARKER_END - READMARKER_START];


void _stm_write_slowpath(object_t *obj)
{
    assert(_running_transaction());

    LIST_APPEND(STM_PSEGMENT->old_objects_to_trace, obj);
    obj->stm_flags |= GCFLAG_WRITE_BARRIER_CALLED;

    /* for old objects from the same transaction, we are done now */
    if (obj_from_same_transaction(obj))
        return;

    /* otherwise, we need to privatize the pages containing the object,
       if they are still SHARED_PAGE.  The common case is that there is
       only one page in total. */
    if (UNLIKELY((obj->stm_flags & GCFLAG_CROSS_PAGE) != 0)) {
        abort();
        //...
    }
    else {
        pages_privatize(((uintptr_t)obj) / 4096UL, 1);
    }

    //... write_locks
    stm_read(obj);
}

static void reset_transaction_read_version(void)
{
    /* force-reset all read markers to 0 */

    /* XXX measure the time taken by this madvise() and the following
       zeroing of pages done lazily by the kernel; compare it with using
       16-bit read_versions.
    */
    /* XXX try to use madvise() on smaller ranges of memory.  In my
       measures, we could gain a factor 2 --- not really more, even if
       the range of virtual addresses below is very large, as long as it
       is already mostly non-reserved pages.  (The following call keeps
       them non-reserved; apparently the kernel just skips them very
       quickly.)
    */
    char *readmarkers = REAL_ADDRESS(STM_SEGMENT->segment_base,
                                     FIRST_READMARKER_PAGE * 4096UL);
    if (madvise(readmarkers, NB_READMARKER_PAGES * 4096UL,
                MADV_DONTNEED) < 0) {
        perror("madvise");
        abort();
    }
    reset_transaction_read_version_prebuilt();
    STM_SEGMENT->transaction_read_version = 1;
}

void _stm_start_transaction(stm_thread_local_t *tl, stm_jmpbuf_t *jmpbuf)
{
    /* GS invalid before this point! */
    acquire_thread_segment(tl);

    STM_SEGMENT->jmpbuf_ptr = jmpbuf;

    uint8_t old_rv = STM_SEGMENT->transaction_read_version;
    STM_SEGMENT->transaction_read_version = old_rv + 1;
    if (UNLIKELY(old_rv == 0xff))
        reset_transaction_read_version();

    assert(list_is_empty(STM_PSEGMENT->old_objects_to_trace));
}


void stm_commit_transaction(void)
{
    stm_thread_local_t *tl = STM_SEGMENT->running_thread;
    release_thread_segment(tl);
}

void stm_abort_transaction(void)
{
    stm_thread_local_t *tl = STM_SEGMENT->running_thread;
    stm_jmpbuf_t *jmpbuf_ptr = STM_SEGMENT->jmpbuf_ptr;
    release_thread_segment(tl);

    assert(jmpbuf_ptr != NULL);
    assert(jmpbuf_ptr != (stm_jmpbuf_t *)-1);    /* for tests only */
    __builtin_longjmp(*jmpbuf_ptr, 1);
}
