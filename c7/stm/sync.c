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
        uint8_t in_use[NB_SEGMENTS];   /* 1 if running a pthread */
        uint64_t global_time;
    };
    char reserved[128];
} sync_ctl __attribute__((aligned(64)));


static void setup_sync(void)
{
    if (pthread_mutex_init(&sync_ctl.global_mutex, NULL) != 0 ||
         pthread_cond_init(&sync_ctl.global_cond, NULL) != 0)
        stm_fatalerror("mutex/cond initialization: %m\n");
}

static void teardown_sync(void)
{
    if (pthread_mutex_destroy(&sync_ctl.global_mutex) != 0 ||
         pthread_cond_destroy(&sync_ctl.global_cond) != 0)
        stm_fatalerror("mutex/cond destroy: %m\n");

    memset(&sync_ctl, 0, sizeof(sync_ctl.in_use));
}

#ifndef NDEBUG
__thread bool _has_mutex_here;
static inline bool _has_mutex(void)
{
    return _has_mutex_here;
}
#endif

static void set_gs_register(char *value)
{
    if (UNLIKELY(syscall(SYS_arch_prctl, ARCH_SET_GS, (uint64_t)value) != 0))
        stm_fatalerror("syscall(arch_prctl, ARCH_SET_GS): %m\n");
}

static inline void mutex_lock(void)
{
    assert(!_has_mutex_here);
    if (UNLIKELY(pthread_mutex_lock(&sync_ctl.global_mutex) != 0))
        stm_fatalerror("pthread_mutex_lock: %m\n");
    assert((_has_mutex_here = true, 1));

    if (STM_PSEGMENT->transaction_state == TS_MUST_ABORT)
        abort_with_mutex();
}

static inline void mutex_unlock(void)
{
    assert(STM_PSEGMENT->safe_point == SP_NO_TRANSACTION ||
           STM_PSEGMENT->safe_point == SP_RUNNING);

    assert(_has_mutex_here);
    if (UNLIKELY(pthread_mutex_unlock(&sync_ctl.global_mutex) != 0))
        stm_fatalerror("pthread_mutex_unlock: %m\n");
    assert((_has_mutex_here = false, 1));
}

static inline void cond_wait(void)
{
#ifdef STM_NO_COND_WAIT
    fprintf(stderr, "*** cond_wait called!");
    abort();
#endif

    assert(_has_mutex_here);
    if (UNLIKELY(pthread_cond_wait(&sync_ctl.global_cond,
                                   &sync_ctl.global_mutex) != 0))
        stm_fatalerror("pthread_cond_wait: %m\n");

    if (STM_PSEGMENT->transaction_state == TS_MUST_ABORT)
        abort_with_mutex();
}

static inline void cond_broadcast(void)
{
    if (UNLIKELY(pthread_cond_broadcast(&sync_ctl.global_cond) != 0))
        stm_fatalerror("pthread_cond_broadcast: %m\n");
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
#ifdef STM_TESTS
        /* that can be optimized away, except during tests, because
           they use only one thread */
        set_gs_register(get_segment_base(num));
#endif
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
    assert(STM_SEGMENT->segment_num == num);
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

static bool _running_transaction(void) __attribute__((unused));
static bool _running_transaction(void)
{
    return (STM_SEGMENT->running_thread != NULL);
}

bool _stm_in_transaction(stm_thread_local_t *tl)
{
    int num = tl->associated_segment_num;
    assert(num < NB_SEGMENTS);
    return get_segment(num)->running_thread == tl;
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

    if (STM_PSEGMENT->transaction_state == TS_MUST_ABORT)
        stm_abort_transaction();
}
#endif


static void wait_for_other_safe_points(int requested_safe_point_kind)
{
    /* Must be called with the mutex.  When all other threads are in a
       safe point of at least the requested kind, returns.  Otherwise,
       asks them to enter a safe point, issues a cond_wait(), and wait.

       When this function returns, the other threads are all blocked at
       safe points as requested.  They may be either in their own
       cond_wait(), or running at SP_NO_TRANSACTION, in which case they
       should not do anything related to stm until the next time they
       call mutex_lock().

       The next time we unlock the mutex (with mutex_unlock() or
       cond_wait()), they will proceed.

       This function requires that the calling thread is in a safe-point
       right now, so there is no deadlock if one thread calls
       wait_for_other_safe_points() while another is currently blocked
       in the cond_wait() in this same function.
    */

 restart:
    assert(_has_mutex());
    assert(STM_PSEGMENT->safe_point == SP_SAFE_POINT_CAN_COLLECT);

    long i;
    for (i = 0; i < NB_SEGMENTS; i++) {
        /* If the other thread is SP_NO_TRANSACTION, then it can be
           ignored here: as long as we have the mutex, it will remain
           SP_NO_TRANSACTION.  If it is already at a suitable safe point,
           it must be in a cond_wait(), so it will not resume as long
           as we hold the mutex.  Thus the only cases is if it is
           SP_RUNNING, or at the wrong kind of safe point.
        */
        struct stm_priv_segment_info_s *other_pseg = get_priv_segment(i);
        if (other_pseg->safe_point == SP_RUNNING ||
            (requested_safe_point_kind == SP_SAFE_POINT_CAN_COLLECT &&
                other_pseg->safe_point == SP_SAFE_POINT_CANNOT_COLLECT)) {

            /* we need to wait for this thread.  Use NSE_SIGNAL to ask
               it (and possibly all other threads in the same case) to
               enter a safe-point soon. */
            _stm_nursery_end = NSE_SIGNAL;
            cond_wait();
            goto restart;
        }
    }

    /* all threads are at a safe-point now. */
    cond_broadcast();   /* to wake up the other threads, but later,
                           when they get the mutex again */
}

void _stm_collectable_safe_point(void)
{
    /* If nursery_section_end was set to NSE_SIGNAL by another thread,
       we end up here as soon as we try to call stm_allocate() or do
       a call to stm_safe_point().
       See wait_for_other_safe_points() for details.
    */
    mutex_lock();
    assert(STM_PSEGMENT->safe_point == SP_RUNNING);

    if (_stm_nursery_end == NSE_SIGNAL) {
        STM_PSEGMENT->safe_point = SP_SAFE_POINT_CAN_COLLECT;

        cond_broadcast();

        do { cond_wait(); } while (_stm_nursery_end == NSE_SIGNAL);

        STM_PSEGMENT->safe_point = SP_RUNNING;
    }

    mutex_unlock();
}
