#include <pthread.h>
#include <sys/syscall.h>
#include <sys/prctl.h>
#include <asm/prctl.h>


/* XXX Getting the most efficient locks is hard, but the following
   simplification is probably good enough for small numbers of threads:
   when a thread wants to check or change any global state (e.g. start
   running a transaction, etc.), it acquires this single mutex.  If
   additionally it wants to wait until the global state is changed by
   someone else, it waits on the condition variable.  This should be
   all we need for synchronization.

   Maybe look at https://github.com/neosmart/pevents for how they do
   WaitForMultipleObjects().
*/


static union {
    struct {
        pthread_mutex_t global_mutex;
        pthread_cond_t global_cond;
        /* some additional pieces of global state follow */
        uint8_t in_use[NB_SEGMENTS + 1];   /* 1 if running a pthread */
        uint64_t global_time;
    };
    char reserved[128];
} sync_ctl __attribute__((aligned(64)));


static void setup_sync(void)
{
    if (pthread_mutex_init(&sync_ctl.global_mutex, NULL) != 0 ||
         pthread_cond_init(&sync_ctl.global_cond, NULL) != 0) {
        perror("mutex/cond initialization");
        abort();
    }
    sync_ctl.in_use[NB_SEGMENTS] = 0xff;
}

static void teardown_sync(void)
{
    if (pthread_mutex_destroy(&sync_ctl.global_mutex) != 0 ||
         pthread_cond_destroy(&sync_ctl.global_cond) != 0) {
        perror("mutex/cond destroy");
        abort();
    }
    memset(&sync_ctl, 0, sizeof(sync_ctl.in_use));
}

static void set_gs_register(char *value)
{
    if (syscall(SYS_arch_prctl, ARCH_SET_GS, (uint64_t)value) != 0) {
        perror("syscall(arch_prctl, ARCH_SET_GS)");
        abort();
    }
}

static inline void mutex_lock(void)
{
    if (UNLIKELY(pthread_mutex_lock(&sync_ctl.global_mutex) != 0)) {
        perror("pthread_mutex_lock");
        abort();
    }
}

static inline void mutex_unlock(void)
{
    assert(STM_PSEGMENT->safe_point == SP_NO_TRANSACTION ||
           STM_PSEGMENT->safe_point == SP_RUNNING);

    if (UNLIKELY(pthread_mutex_unlock(&sync_ctl.global_mutex) != 0)) {
        perror("pthread_mutex_unlock");
        abort();
    }
}

static inline bool _has_mutex(void)
{
    return pthread_mutex_trylock(&sync_ctl.global_mutex) == EBUSY;
}

static inline void cond_wait(void)
{
#ifdef STM_NO_COND_WAIT
    fprintf(stderr, "*** cond_wait called!");
    abort();
#endif

    if (UNLIKELY(pthread_cond_wait(&sync_ctl.global_cond,
                                   &sync_ctl.global_mutex) != 0)) {
        perror("pthread_cond_wait");
        abort();
    }

    if (STM_PSEGMENT->transaction_state == TS_MUST_ABORT)
        abort_with_mutex();
}

static inline void cond_broadcast(void)
{
    if (UNLIKELY(pthread_cond_broadcast(&sync_ctl.global_cond) != 0)) {
        perror("pthread_cond_broadcast");
        abort();
    }
}

static void acquire_thread_segment(stm_thread_local_t *tl)
{
    /* This function acquires a segment for the currently running thread,
       and set up the GS register if it changed. */
    assert(_has_mutex());
    assert(_is_tl_registered(tl));

 retry:;
    int num = tl->associated_segment_num;
    if (sync_ctl.in_use[num] == 0) {
        /* fast-path: we can get the same segment number than the one
           we had before.  The value stored in GS is still valid. */
        goto got_num;
    }
    /* Look for the next free segment.  If there is none, wait for
       the condition variable. */
    int i;
    for (i = 0; i < NB_SEGMENTS; i++) {
        num = (num + 1) % NB_SEGMENTS;
        if (sync_ctl.in_use[num] == 0) {
            /* we're getting 'num', a different number. */
            tl->associated_segment_num = num;
            set_gs_register(get_segment_base(num));
            goto got_num;
        }
    }
    /* Wait and retry */
    cond_wait();
    goto retry;

 got_num:
    sync_ctl.in_use[num] = 1;
    assert(STM_SEGMENT->running_thread == NULL);
    STM_SEGMENT->running_thread = tl;
    STM_PSEGMENT->start_time = ++sync_ctl.global_time;
}

static void release_thread_segment(stm_thread_local_t *tl)
{
    assert(_has_mutex());

    assert(STM_SEGMENT->running_thread == tl);
    STM_SEGMENT->running_thread = NULL;

    assert(sync_ctl.in_use[tl->associated_segment_num] == 1);
    sync_ctl.in_use[tl->associated_segment_num] = 0;
}

static bool _running_transaction(void)
{
    return (STM_SEGMENT->running_thread != NULL);
}

bool _stm_in_transaction(stm_thread_local_t *tl)
{
    int num = tl->associated_segment_num;
    if (num < NB_SEGMENTS)
        return get_segment(num)->running_thread == tl;
    else
        return false;
}

void _stm_test_switch(stm_thread_local_t *tl)
{
    assert(_stm_in_transaction(tl));
    set_gs_register(get_segment_base(tl->associated_segment_num));
    assert(STM_SEGMENT->running_thread == tl);
}

#if STM_TESTS
void _stm_start_safe_point(void)
{
    assert(STM_PSEGMENT->safe_point == SP_RUNNING);
    STM_PSEGMENT->safe_point = SP_SAFE_POINT_CAN_COLLECT;
}

void _stm_stop_safe_point(void)
{
    assert(STM_PSEGMENT->safe_point == SP_SAFE_POINT_CAN_COLLECT);
    STM_PSEGMENT->safe_point = SP_RUNNING;

    restore_nursery_section_end(NSE_SIGNAL_DONE);
    if (STM_PSEGMENT->transaction_state == TS_MUST_ABORT)
        stm_abort_transaction();
}
#endif


static bool try_wait_for_other_safe_points(int requested_safe_point_kind)
{
    /* Must be called with the mutex.  If all other threads are in a
       safe point of at least the requested kind, returns true.  Otherwise,
       asks them to enter a safe point, issues a cond_wait(), and returns
       false; you can call repeatedly this function in this case.

       When this function returns true, the other threads are all
       blocked at safe points as requested.  They may be either in their
       own cond_wait(), or running at SP_NO_TRANSACTION, in which case
       they should not do anything related to stm until the next time
       they call mutex_lock().

       The next time we unlock the mutex (with mutex_unlock() or
       cond_wait()), they will proceed.

       This function requires that the calling thread is in a safe-point
       right now, so there is no deadlock if one thread calls
       try_wait_for_other_safe_points() while another is currently blocked
       in the cond_wait() in this same function.
    */
    assert(_has_mutex());
    assert(STM_PSEGMENT->safe_point == SP_SAFE_POINT_CAN_COLLECT);

    long i;
    bool must_wait = false;
    for (i = 0; i < NB_SEGMENTS; i++) {
        if (i == STM_SEGMENT->segment_num)
            continue;    /* ignore myself */

        struct stm_priv_segment_info_s *other_pseg = get_priv_segment(i);
        if (other_pseg->safe_point == SP_RUNNING ||
            (requested_safe_point_kind == SP_SAFE_POINT_CAN_COLLECT &&
                other_pseg->safe_point == SP_SAFE_POINT_CANNOT_COLLECT)) {

            /* we need to wait for this thread.  Use NSE_SIGNAL to
               ask it to enter a safe-point soon. */
            other_pseg->pub.v_nursery_section_end = NSE_SIGNAL;
            must_wait = true;
        }
    }
    if (must_wait) {
        cond_wait();
        return false;
    }

    /* done!  All NSE_SIGNAL threads become NSE_SIGNAL_DONE now, which
       mean they will actually run again the next time they grab the
       mutex. */
    for (i = 0; i < NB_SEGMENTS; i++) {
        if (i == STM_SEGMENT->segment_num)
            continue;    /* ignore myself */

        struct stm_segment_info_s *other_seg = get_segment(i);
        if (other_seg->v_nursery_section_end == NSE_SIGNAL)
            other_seg->v_nursery_section_end = NSE_SIGNAL_DONE;
    }
    cond_broadcast();   /* to wake up the other threads, but later,
                           when they get the mutex again */
    return true;
}

static bool collectable_safe_point(void)
{
    bool any_operation = false;
 restart:;
    switch (STM_SEGMENT->v_nursery_section_end) {

    case NSE_SIGNAL:
        /* If nursery_section_end was set to NSE_SIGNAL by another thread,
           we end up here as soon as we try to call stm_allocate().
           See try_wait_for_other_safe_points() for details. */
        mutex_lock();
        assert(STM_PSEGMENT->safe_point == SP_RUNNING);
        STM_PSEGMENT->safe_point = SP_SAFE_POINT_CAN_COLLECT;
        cond_broadcast();
        cond_wait();
        STM_PSEGMENT->safe_point = SP_RUNNING;
        mutex_unlock();

        /* Once the sync point is done, retry. */
        any_operation = true;
        goto restart;

    case NSE_SIGNAL_DONE:
        restore_nursery_section_end(NSE_SIGNAL_DONE);
        any_operation = true;
        break;

    default:;
    }
    return any_operation;
}
