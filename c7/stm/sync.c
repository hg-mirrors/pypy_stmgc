#include <sys/syscall.h>
#include <sys/prctl.h>
#include <asm/prctl.h>


/* Each segment can be in one of three possible states, described by
   the segment variable 'safe_point':

   - SP_NO_TRANSACTION: no thread is running any transaction using this
     segment.

   - SP_RUNNING: a thread is running a transaction using this segment.

   - SP_WAIT_FOR_xxx: the thread that owns this segment is currently
     suspended in a safe-point.  (A safe-point means that it is not
     changing anything right now, and the current shadowstack is correct.)

   Synchronization is done with a single mutex and a few condition
   variables.  A thread needs to have acquired the mutex in order to do
   things like acquiring or releasing ownership of a segment or updating
   this segment's state.  No other thread can acquire the mutex
   concurrently, and so there is no race: the (single) thread owning the
   mutex can freely inspect or even change the state of other segments
   too.
*/


static union {
    struct {
        pthread_mutex_t global_mutex;
        pthread_cond_t cond[_C_TOTAL];
        /* some additional pieces of global state follow */
        uint8_t in_use[NB_SEGMENTS];   /* 1 if running a pthread */
        uint64_t global_time;
    };
    char reserved[192];
} sync_ctl __attribute__((aligned(64)));


static void setup_sync(void)
{
    if (pthread_mutex_init(&sync_ctl.global_mutex, NULL) != 0)
        stm_fatalerror("mutex initialization: %m\n");

    long i;
    for (i = 0; i < _C_TOTAL; i++) {
        if (pthread_cond_init(&sync_ctl.cond[i], NULL) != 0)
            stm_fatalerror("cond initialization: %m\n");
    }
}

static void teardown_sync(void)
{
    if (pthread_mutex_destroy(&sync_ctl.global_mutex) != 0)
        stm_fatalerror("mutex destroy: %m\n");

    long i;
    for (i = 0; i < _C_TOTAL; i++) {
        if (pthread_cond_destroy(&sync_ctl.cond[i]) != 0)
            stm_fatalerror("cond destroy: %m\n");
    }

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

static inline void s_mutex_lock(void)
{
    assert(!_has_mutex_here);
    if (UNLIKELY(pthread_mutex_lock(&sync_ctl.global_mutex) != 0))
        stm_fatalerror("pthread_mutex_lock: %m\n");
    assert((_has_mutex_here = true, 1));
}

static inline void s_mutex_unlock(void)
{
    assert(_has_mutex_here);
    if (UNLIKELY(pthread_mutex_unlock(&sync_ctl.global_mutex) != 0))
        stm_fatalerror("pthread_mutex_unlock: %m\n");
    assert((_has_mutex_here = false, 1));
}

static inline void cond_wait(enum cond_type_e ctype)
{
#ifdef STM_NO_COND_WAIT
    stm_fatalerror("*** cond_wait/%d called!\n", (int)ctype);
#endif

    assert(_has_mutex_here);
    if (UNLIKELY(pthread_cond_wait(&sync_ctl.cond[ctype],
                                   &sync_ctl.global_mutex) != 0))
        stm_fatalerror("pthread_cond_wait/%d: %m\n", (int)ctype);
}

static inline void cond_signal(enum cond_type_e ctype)
{
    if (UNLIKELY(pthread_cond_signal(&sync_ctl.cond[ctype]) != 0))
        stm_fatalerror("pthread_cond_signal/%d: %m\n", (int)ctype);
}

static inline void cond_broadcast(enum cond_type_e ctype)
{
    if (UNLIKELY(pthread_cond_broadcast(&sync_ctl.cond[ctype]) != 0))
        stm_fatalerror("pthread_cond_broadcast/%d: %m\n", (int)ctype);
}

/************************************************************/


static void wait_for_end_of_inevitable_transaction(bool can_abort)
{
    long i;
 restart:
    for (i = 0; i < NB_SEGMENTS; i++) {
        if (get_priv_segment(i)->transaction_state == TS_INEVITABLE) {
            if (can_abort) {
                /* for now, always abort if we can.  We could also try
                   sometimes to wait for the other thread (needs to
                   take care about setting safe_point then) */
                abort_with_mutex();
            }
            /* wait for stm_commit_transaction() to finish this
               inevitable transaction */
            cond_wait(C_INEVITABLE);
            goto restart;
        }
    }
}

static bool acquire_thread_segment(stm_thread_local_t *tl)
{
    /* This function acquires a segment for the currently running thread,
       and set up the GS register if it changed. */
    assert(_has_mutex());
    assert(_is_tl_registered(tl));

    int num = tl->associated_segment_num;
    if (sync_ctl.in_use[num] == 0) {
        /* fast-path: we can get the same segment number than the one
           we had before.  The value stored in GS is still valid. */
#ifdef STM_TESTS
        /* that can be optimized away, except during tests, because
           they use only one thread */
        set_gs_register(get_segment_base(num));
#endif
        dprintf(("acquired same segment: %d\n", num));
        goto got_num;
    }
    /* Look for the next free segment.  If there is none, wait for
       the condition variable. */
    int i;
    for (i = 0; i < NB_SEGMENTS; i++) {
        num = (num + 1) % NB_SEGMENTS;
        if (sync_ctl.in_use[num] == 0) {
            /* we're getting 'num', a different number. */
            dprintf(("acquired different segment: %d->%d\n", tl->associated_segment_num, num));
            tl->associated_segment_num = num;
            set_gs_register(get_segment_base(num));
            goto got_num;
        }
    }
    /* No segment available.  Wait until release_thread_segment()
       signals that one segment has been freed. */
    cond_wait(C_SEGMENT_FREE);

    /* Return false to the caller, which will call us again */
    return false;

 got_num:
    sync_ctl.in_use[num] = 1;
    assert(STM_SEGMENT->segment_num == num);
    assert(STM_SEGMENT->running_thread == NULL);
    STM_SEGMENT->running_thread = tl;
    STM_PSEGMENT->start_time = ++sync_ctl.global_time;
    return true;
}

static void release_thread_segment(stm_thread_local_t *tl)
{
    assert(_has_mutex());

    assert(STM_SEGMENT->running_thread == tl);
    STM_SEGMENT->running_thread = NULL;

    assert(sync_ctl.in_use[tl->associated_segment_num] == 1);
    sync_ctl.in_use[tl->associated_segment_num] = 0;

    /* wake up one of the threads waiting in acquire_thread_segment() */
    cond_signal(C_SEGMENT_FREE);
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

    long i;
    for (i = 0; i < NB_SEGMENTS; i++) {
        if (get_segment(i)->nursery_end == NURSERY_END)
            get_segment(i)->nursery_end = NSE_SIGPAUSE;
    }
}

static inline long count_other_threads_sp_running(void)
{
    /* Return the number of other threads in SP_RUNNING.
       Asserts that SP_RUNNING threads still have the NSE_SIGxxx. */
    long i;
    long result = 0;
    int my_num = STM_SEGMENT->segment_num;

    for (i = 0; i < NB_SEGMENTS; i++) {
        if (i != my_num && get_priv_segment(i)->safe_point == SP_RUNNING) {
            assert(get_segment(i)->nursery_end <= _STM_NSE_SIGNAL_MAX);
            result++;
        }
    }
    return result;
}

static void remove_requests_for_safe_point(void)
{
    assert(_safe_points_requested == true);
    assert((_safe_points_requested = false, 1));

    long i;
    for (i = 0; i < NB_SEGMENTS; i++) {
        assert(get_segment(i)->nursery_end != NURSERY_END);
        if (get_segment(i)->nursery_end == NSE_SIGPAUSE)
            get_segment(i)->nursery_end = NURSERY_END;
    }
    cond_broadcast(C_REQUEST_REMOVED);
}

static void enter_safe_point_if_requested(void)
{
    assert(_has_mutex());
    while (1) {
        if (must_abort())
            abort_with_mutex();

        if (STM_SEGMENT->nursery_end == NURSERY_END)
            break;    /* no safe point requested */

        assert(STM_SEGMENT->nursery_end == NSE_SIGPAUSE);

        /* If we are requested to enter a safe-point, we cannot proceed now.
           Wait until the safe-point request is removed for us. */

        cond_signal(C_AT_SAFE_POINT);
        STM_PSEGMENT->safe_point = SP_WAIT_FOR_C_REQUEST_REMOVED;
        cond_wait(C_REQUEST_REMOVED);
        STM_PSEGMENT->safe_point = SP_RUNNING;
    }
}

static void synchronize_all_threads(void)
{
    enter_safe_point_if_requested();

    /* Only one thread should reach this point concurrently.  This is
       why: if several threads call this function, the first one that
       goes past this point will set the "request safe point" on all
       other threads; then none of the other threads will go past the
       enter_safe_point_if_requested() above. */
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

    /* Remove the requests for safe-points now.  In principle we should
       remove it later, when the caller is done, but this is equivalent
       as long as we hold the mutex.
    */
    remove_requests_for_safe_point();    /* => C_REQUEST_REMOVED */
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
