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
    memset(sync_ctl, 0, sizeof(sync_ctl.in_use));
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
    if (UNLIKELY(pthread_mutex_unlock(&sync_ctl.global_mutex) != 0)) {
        perror("pthread_mutex_unlock");
        abort();
    }
}

static inline void assert_has_mutex(void)
{
    assert(pthread_mutex_trylock(&sync_ctl.global_mutex) == EBUSY);
}

static inline void cond_wait(void)
{
    if (UNLIKELY(pthread_cond_wait(&sync_ctl.global_cond,
                                   &sync_ctl.global_mutex) != 0)) {
        perror("pthread_cond_wait");
        abort();
    }
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
    assert_has_mutex();
    assert(_is_tl_registered(tl));

 retry:
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
    STM_PSEGMENT->start_time = ++segments_ctl.global_time;
}

static void release_thread_segment(stm_thread_local_t *tl)
{
    assert_has_mutex();

    assert(STM_SEGMENT->running_thread == tl);
    STM_SEGMENT->running_thread = NULL;

    assert(sync_ctl.in_use[tl->associated_segment_num] == 1);
    sync_ctl.in_use[tl->associated_segment_num] = 0;

    cond_broadcast();
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

void _stm_start_safe_point(int flags)
{
    //...
}

void _stm_stop_safe_point(int flags)
{
    //...
}
