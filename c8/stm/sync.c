#include <sys/syscall.h>
#include <sys/prctl.h>
#include <asm/prctl.h>

#ifndef _STM_CORE_H_
# error "must be compiled via stmgc.c"
#endif



static union {
    struct {
        pthread_mutex_t global_mutex;
        pthread_cond_t cond[_C_TOTAL];
        /* some additional pieces of global state follow */
        uint8_t in_use1[NB_SEGMENTS];   /* 1 if running a pthread, idx=0 unused */
    };
    char reserved[192];
} sync_ctl __attribute__((aligned(64)));


static void setup_sync(void)
{
    if (pthread_mutex_init(&sync_ctl.global_mutex, NULL) != 0)
        stm_fatalerror("mutex initialization: %m");

    long i;
    for (i = 0; i < _C_TOTAL; i++) {
        if (pthread_cond_init(&sync_ctl.cond[i], NULL) != 0)
            stm_fatalerror("cond initialization: %m");
    }
}

static void teardown_sync(void)
{
    if (pthread_mutex_destroy(&sync_ctl.global_mutex) != 0)
        stm_fatalerror("mutex destroy: %m");

    long i;
    for (i = 0; i < _C_TOTAL; i++) {
        if (pthread_cond_destroy(&sync_ctl.cond[i]) != 0)
            stm_fatalerror("cond destroy: %m");
    }

    memset(&sync_ctl, 0, sizeof(sync_ctl));
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
        stm_fatalerror("syscall(arch_prctl, ARCH_SET_GS): %m");
}

static inline void s_mutex_lock(void)
{
    assert(!_has_mutex_here);
    if (UNLIKELY(pthread_mutex_lock(&sync_ctl.global_mutex) != 0))
        stm_fatalerror("pthread_mutex_lock: %m");
    assert((_has_mutex_here = true, 1));
}

static inline void s_mutex_unlock(void)
{
    assert(_has_mutex_here);
    if (UNLIKELY(pthread_mutex_unlock(&sync_ctl.global_mutex) != 0))
        stm_fatalerror("pthread_mutex_unlock: %m");
    assert((_has_mutex_here = false, 1));
}


static inline void cond_wait(enum cond_type_e ctype)
{
#ifdef STM_NO_COND_WAIT
    stm_fatalerror("*** cond_wait/%d called!", (int)ctype);
#endif

    assert(_has_mutex_here);
    if (UNLIKELY(pthread_cond_wait(&sync_ctl.cond[ctype],
                                   &sync_ctl.global_mutex) != 0))
        stm_fatalerror("pthread_cond_wait/%d: %m", (int)ctype);
}

static inline void cond_signal(enum cond_type_e ctype)
{
    if (UNLIKELY(pthread_cond_signal(&sync_ctl.cond[ctype]) != 0))
        stm_fatalerror("pthread_cond_signal/%d: %m", (int)ctype);
}

static inline void cond_broadcast(enum cond_type_e ctype)
{
    if (UNLIKELY(pthread_cond_broadcast(&sync_ctl.cond[ctype]) != 0))
        stm_fatalerror("pthread_cond_broadcast/%d: %m", (int)ctype);
}

/************************************************************/


static bool acquire_thread_segment(stm_thread_local_t *tl)
{
    /* This function acquires a segment for the currently running thread,
       and set up the GS register if it changed. */
    assert(_has_mutex());
    assert(_is_tl_registered(tl));

    int num = tl->last_associated_segment_num - 1; // 0..NB_SEG-1
    OPT_ASSERT(num >= 0);
    if (sync_ctl.in_use1[num+1] == 0) {
        /* fast-path: we can get the same segment number than the one
           we had before.  The value stored in GS is still valid. */
#ifdef STM_TESTS
        /* that can be optimized away, except during tests, because
           they use only one thread */
        set_gs_register(get_segment_base(num+1));
#endif
        dprintf(("acquired same segment: %d\n", num+1));
        goto got_num;
    }
    /* Look for the next free segment.  If there is none, wait for
       the condition variable. */
    int retries;
    for (retries = 0; retries < NB_SEGMENTS-1; retries++) {
        num = (num+1) % (NB_SEGMENTS-1);
        if (sync_ctl.in_use1[num+1] == 0) {
            /* we're getting 'num', a different number. */
            dprintf(("acquired different segment: %d->%d\n",
                     tl->last_associated_segment_num, num+1));
            tl->last_associated_segment_num = num+1;
            set_gs_register(get_segment_base(num+1));
            goto got_num;
        }
    }
    /* No segment available.  Wait until release_thread_segment()
       signals that one segment has been freed. */
    cond_wait(C_SEGMENT_FREE);

    /* Return false to the caller, which will call us again */
    return false;
 got_num:
    OPT_ASSERT(num >= 0 && num < NB_SEGMENTS-1);
    sync_ctl.in_use1[num+1] = 1;
    assert(STM_SEGMENT->segment_num == num+1);
    assert(STM_SEGMENT->running_thread == NULL);
    tl->associated_segment_num = tl->last_associated_segment_num;
    STM_SEGMENT->running_thread = tl;
    return true;
}

static void release_thread_segment(stm_thread_local_t *tl)
{
    assert(_has_mutex());

    cond_signal(C_SEGMENT_FREE);

    assert(STM_SEGMENT->running_thread == tl);
    assert(tl->associated_segment_num == tl->last_associated_segment_num);
    tl->associated_segment_num = -1;
    STM_SEGMENT->running_thread = NULL;

    assert(sync_ctl.in_use1[tl->last_associated_segment_num] == 1);
    sync_ctl.in_use1[tl->last_associated_segment_num] = 0;

    /* check for cle collection here after we released the segment
       while we still hold the mutex */
    maybe_collect_commit_log();
}

__attribute__((unused))
static bool _seems_to_be_running_transaction(void)
{
    return (STM_SEGMENT->running_thread != NULL);
}

bool _stm_in_transaction(stm_thread_local_t *tl)
{
    if (tl->associated_segment_num == -1) {
        return false;
    }
    else {
        int num = tl->associated_segment_num;
        OPT_ASSERT(1 <= num && num < NB_SEGMENTS);
        OPT_ASSERT(num == tl->last_associated_segment_num);
        OPT_ASSERT(get_segment(num)->running_thread == tl);
        return true;
    }
}

void _stm_test_switch(stm_thread_local_t *tl)
{
    assert(_stm_in_transaction(tl));
    set_gs_register(get_segment_base(tl->associated_segment_num));
    assert(STM_SEGMENT->running_thread == tl);
    exec_local_finalizers();
}

void _stm_test_switch_segment(int segnum)
{
    set_gs_register(get_segment_base(segnum+1));
}

#if STM_TESTS
void _stm_start_safe_point(void)
{
    assert(STM_PSEGMENT->safe_point == SP_RUNNING);
    STM_PSEGMENT->safe_point = SP_WAIT_FOR_OTHER_THREAD;
}

void _stm_stop_safe_point(void)
{
    assert(STM_PSEGMENT->safe_point == SP_WAIT_FOR_OTHER_THREAD);
    STM_PSEGMENT->safe_point = SP_RUNNING;

    stm_safe_point();
}
#endif



/************************************************************/


#ifndef NDEBUG
static bool _safe_points_requested = false;
#endif

static void signal_everybody_to_pause_running(void)
{
    assert(_safe_points_requested == false);
    assert((_safe_points_requested = true, 1));
    assert(_has_mutex());

    long i;
    for (i = 1; i < NB_SEGMENTS; i++) {
        if (get_segment(i)->nursery_end == NURSERY_END)
            get_segment(i)->nursery_end = NSE_SIGPAUSE;
    }
    assert(!pause_signalled);
    pause_signalled = true;
}

static inline long count_other_threads_sp_running(void)
{
    /* Return the number of other threads in SP_RUNNING.
       Asserts that SP_RUNNING threads still have the NSE_SIGxxx. */
    long i;
    long result = 0;
    int my_num = STM_SEGMENT->segment_num;

    for (i = 1; i < NB_SEGMENTS; i++) {
        if (i != my_num && get_priv_segment(i)->safe_point == SP_RUNNING) {
            assert(get_segment(i)->nursery_end <= _STM_NSE_SIGNAL_MAX);
            result++;
        }
    }
    return result;
}

static void remove_requests_for_safe_point(void)
{
    assert(pause_signalled);
    pause_signalled = false;
    assert(_safe_points_requested == true);
    assert((_safe_points_requested = false, 1));

    long i;
    for (i = 1; i < NB_SEGMENTS; i++) {
        assert(get_segment(i)->nursery_end != NURSERY_END);
        if (get_segment(i)->nursery_end == NSE_SIGPAUSE)
            get_segment(i)->nursery_end = NURSERY_END;
    }
    cond_broadcast(C_REQUEST_REMOVED);
}

static void enter_safe_point_if_requested(void)
{
    if (STM_SEGMENT->nursery_end == NURSERY_END)
        return;    /* fast path: no safe point requested */

    assert(_seems_to_be_running_transaction());
    assert(_has_mutex());
    while (1) {
        if (must_abort())
            abort_with_mutex();

        if (STM_SEGMENT->nursery_end == NURSERY_END)
            break;    /* no safe point requested */

        assert(STM_SEGMENT->nursery_end == NSE_SIGPAUSE);
        assert(pause_signalled);

        /* If we are requested to enter a safe-point, we cannot proceed now.
           Wait until the safe-point request is removed for us. */
#ifdef STM_TESTS
        abort_with_mutex();
#endif
        cond_signal(C_AT_SAFE_POINT);
        STM_PSEGMENT->safe_point = SP_WAIT_FOR_C_REQUEST_REMOVED;
        cond_wait(C_REQUEST_REMOVED);
        STM_PSEGMENT->safe_point = SP_RUNNING;
    }
}

static void synchronize_all_threads(enum sync_type_e sync_type)
{
    enter_safe_point_if_requested();

    /* Only one thread should reach this point concurrently.  This is
       why: if several threads call this function, the first one that
       goes past this point will set the "request safe point" on all
       other threads; then none of the other threads will go past the
       enter_safe_point_if_requested() above.
    */
    if (UNLIKELY(globally_unique_transaction)) {
        assert(count_other_threads_sp_running() == 0);
        return;
    }

    signal_everybody_to_pause_running();

    /* If some other threads are SP_RUNNING, we cannot proceed now.
       Wait until all other threads are suspended. */
    while (count_other_threads_sp_running() > 0) {
        STM_PSEGMENT->safe_point = SP_WAIT_FOR_C_AT_SAFE_POINT;
        cond_wait(C_AT_SAFE_POINT);
        STM_PSEGMENT->safe_point = SP_RUNNING;

        if (must_abort()) {
            remove_requests_for_safe_point();    /* => C_REQUEST_REMOVED */
            abort_with_mutex();
        }
    }

    if (UNLIKELY(sync_type == STOP_OTHERS_AND_BECOME_GLOBALLY_UNIQUE)) {
        globally_unique_transaction = true;
        assert(STM_SEGMENT->nursery_end == NSE_SIGPAUSE);
        STM_SEGMENT->nursery_end = NURSERY_END;
        return;  /* don't remove the requests for safe-points in this case */
    }

    /* Remove the requests for safe-points now.  In principle we should
       remove it later, when the caller is done, but this is equivalent
       as long as we hold the mutex.
    */
    remove_requests_for_safe_point();    /* => C_REQUEST_REMOVED */
}

static void committed_globally_unique_transaction(void)
{
    assert(globally_unique_transaction);
    assert(STM_SEGMENT->nursery_end == NURSERY_END);
    STM_SEGMENT->nursery_end = NSE_SIGPAUSE;
    globally_unique_transaction = false;
    remove_requests_for_safe_point();
}

void _stm_collectable_safe_point(void)
{
    /* If 'nursery_end' was set to NSE_SIGxxx by another thread,
       we end up here as soon as we try to call stm_allocate() or do
       a call to stm_safe_point().
    */
    s_mutex_lock();
    enter_safe_point_if_requested();
    s_mutex_unlock();
}
