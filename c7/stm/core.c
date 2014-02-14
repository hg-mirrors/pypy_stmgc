#ifndef _STM_CORE_H_
# error "must be compiled via stmgc.c"
#endif

#include <unistd.h>


static uint8_t write_locks[READMARKER_END - READMARKER_START];

static void teardown_core(void)
{
    memset(write_locks, 0, sizeof(write_locks));
}


static void contention_management(uint8_t current_lock_owner)
{
    /* A simple contention manager.  Called when we do stm_write()
       on an object, but some other thread already holds the write
       lock on the same object. */

    /* By construction it should not be possible that the owner
       of the object is precisely us */
    assert(current_lock_owner != STM_PSEGMENT->write_lock_num);

    /* Who should abort here: this thread, or the other thread? */
    struct stm_priv_segment_info_s* other_pseg;
    other_pseg = get_priv_segment(current_lock_owner - 1);
    assert(other_pseg->write_lock_num == current_lock_owner);

    if ((STM_PSEGMENT->approximate_start_time <
            other_pseg->approximate_start_time) || is_inevitable()) {
        /* we are the thread that must succeed */
        other_pseg->need_abort = 1;
        _stm_start_safe_point(0);
        /* XXX: not good, maybe should be signalled by other thread */
        usleep(1);
        _stm_stop_safe_point(0);
        /* done, will retry */
    }
    else {
        /* we are the thread that must abort */
        stm_abort_transaction();
    }
}


void _stm_write_slowpath(object_t *obj)
{
    assert(_running_transaction());

    LIST_APPEND(STM_PSEGMENT->old_objects_to_trace, obj);

    /* for old objects from the same transaction, we are done now */
    if (obj_from_same_transaction(obj)) {
        obj->stm_flags |= GCFLAG_WRITE_BARRIER_CALLED;
        return;
    }

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

    /* claim the write-lock for this object */
    do {
        uintptr_t lock_idx = (((uintptr_t)obj) >> 4) - READMARKER_START;
        uint8_t lock_num = STM_PSEGMENT->write_lock_num;
        uint8_t prev_owner;
        prev_owner = __sync_val_compare_and_swap(&write_locks[lock_idx],
                                                 0, lock_num);

        /* if there was no lock-holder, we are done */
        if (LIKELY(prev_owner == 0))
            break;

        /* otherwise, call the contention manager, and then possibly retry */
        contention_management(prev_owner);
    } while (1);

    /* add the write-barrier-already-called flag ONLY if we succeeded in
       getting the write-lock */
    stm_read(obj);
    obj->stm_flags |= GCFLAG_WRITE_BARRIER_CALLED;
    LIST_APPEND(STM_PSEGMENT->modified_objects, obj);
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
    assert(list_is_empty(STM_PSEGMENT->modified_objects));
    assert(list_is_empty(STM_PSEGMENT->creation_markers));
}


void stm_commit_transaction(void)
{
    stm_thread_local_t *tl = STM_SEGMENT->running_thread;
    release_thread_segment(tl);
    reset_all_creation_markers();
}

void stm_abort_transaction(void)
{
    stm_thread_local_t *tl = STM_SEGMENT->running_thread;
    stm_jmpbuf_t *jmpbuf_ptr = STM_SEGMENT->jmpbuf_ptr;
    release_thread_segment(tl);
    reset_all_creation_markers();

    assert(jmpbuf_ptr != NULL);
    assert(jmpbuf_ptr != (stm_jmpbuf_t *)-1);    /* for tests only */
    __builtin_longjmp(*jmpbuf_ptr, 1);
}
