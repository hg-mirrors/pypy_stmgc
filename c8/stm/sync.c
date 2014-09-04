#include <sys/syscall.h>
#include <sys/prctl.h>
#include <asm/prctl.h>

#ifndef _STM_CORE_H_
# error "must be compiled via stmgc.c"
#endif



static union {
    struct {
        pthread_mutex_t global_mutex;
        /* some additional pieces of global state follow */
        uint8_t in_use1[NB_SEGMENTS];   /* 1 if running a pthread */
    };
    char reserved[192];
} sync_ctl __attribute__((aligned(64)));


static void setup_sync(void)
{
    if (pthread_mutex_init(&sync_ctl.global_mutex, NULL) != 0)
        stm_fatalerror("mutex initialization: %m");
}

static void teardown_sync(void)
{
    if (pthread_mutex_destroy(&sync_ctl.global_mutex) != 0)
        stm_fatalerror("mutex destroy: %m");

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

/************************************************************/


static bool acquire_thread_segment(stm_thread_local_t *tl)
{
    /* This function acquires a segment for the currently running thread,
       and set up the GS register if it changed. */
    assert(_has_mutex());
    assert(_is_tl_registered(tl));

    int num = tl->associated_segment_num;
    if (sync_ctl.in_use1[num] == 0) {
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
    int retries;
    for (retries = 0; retries < NB_SEGMENTS; retries++) {
        num = num % NB_SEGMENTS;
        if (sync_ctl.in_use1[num] == 0) {
            /* we're getting 'num', a different number. */
            dprintf(("acquired different segment: %d->%d\n", tl->associated_segment_num, num));
            tl->associated_segment_num = num;
            set_gs_register(get_segment_base(num));
            goto got_num;
        }
    }
    /* No segment available.  Wait until release_thread_segment()
       signals that one segment has been freed. */
    abort();                    /* XXX */

    /* Return false to the caller, which will call us again */
    return false;

 got_num:
    sync_ctl.in_use1[num] = 1;
    assert(STM_SEGMENT->segment_num == num);
    assert(STM_SEGMENT->running_thread == NULL);
    STM_SEGMENT->running_thread = tl;
    return true;
}

static void release_thread_segment(stm_thread_local_t *tl)
{
    assert(_has_mutex());

    assert(STM_SEGMENT->running_thread == tl);
    STM_SEGMENT->running_thread = NULL;

    assert(sync_ctl.in_use1[tl->associated_segment_num] == 1);
    sync_ctl.in_use1[tl->associated_segment_num] = 0;
}

__attribute__((unused))
static bool _seems_to_be_running_transaction(void)
{
    return (STM_SEGMENT->running_thread != NULL);
}

bool _stm_in_transaction(stm_thread_local_t *tl)
{
    int num = tl->associated_segment_num;
    assert(0 <= num && num < NB_SEGMENTS);
    return get_segment(num)->running_thread == tl;
}

void _stm_test_switch(stm_thread_local_t *tl)
{
    assert(_stm_in_transaction(tl));
    set_gs_register(get_segment_base(tl->associated_segment_num));
    assert(STM_SEGMENT->running_thread == tl);
}

void _stm_test_switch_segment(int segnum)
{
    set_gs_register(get_segment_base(segnum));
}
