#include <semaphore.h>
#include <sys/syscall.h>
#include <sys/prctl.h>
#include <asm/prctl.h>


static union {
    struct {
        sem_t semaphore;
        uint8_t in_use[NB_SEGMENTS];   /* 1 if running a pthread */
    };
    char reserved[64];
} segments_ctl __attribute__((aligned(64)));


static void setup_sync(void)
{
    memset(segments_ctl.in_use, 0, sizeof(segments_ctl.in_use));
    if (sem_init(&segments_ctl.semaphore, 0, NB_SEGMENTS) != 0) {
        perror("sem_init");
        abort();
    }
}

static void teardown_sync(void)
{
    if (sem_destroy(&segments_ctl.semaphore) != 0) {
        perror("sem_destroy");
        abort();
    }
}

static void set_gs_register(char *value)
{
    if (syscall(SYS_arch_prctl, ARCH_SET_GS, (uint64_t)value) != 0) {
        perror("syscall(arch_prctl, ARCH_SET_GS)");
        abort();
    }
}

static void acquire_thread_segment(stm_thread_local_t *tl)
{
    /* This function acquires a segment for the currently running thread,
       and set up the GS register if it changed. */
    while (sem_wait(&segments_ctl.semaphore) != 0) {
        if (errno != EINTR) {
            perror("sem_wait");
            abort();
        }
    }
    int num = tl->associated_segment_num;
    if (num >= 0) {
        if (__sync_lock_test_and_set(&segments_ctl.in_use[num], 1) == 0) {
            /* fast-path: reacquired the same segment number than the one
               we had.  The value stored in GS is still valid. */
            goto exit;
        }
    }
    /* Look for the next free segment.  There must be one, because we
       acquired the semaphore above. */
    while (1) {
        num = (num + 1) % NB_SEGMENTS;
        if (__sync_lock_test_and_set(&segments_ctl.in_use[num], 1) == 0)
            break;
    }
    tl->associated_segment_num = num;
    set_gs_register(get_segment_base(num));

 exit:
    assert(STM_SEGMENT->running_thread == NULL);
    STM_SEGMENT->running_thread = tl;
}

static void release_thread_segment(stm_thread_local_t *tl)
{
    assert(STM_SEGMENT->running_thread == tl);
    STM_SEGMENT->running_thread = NULL;

    int num = tl->associated_segment_num;
    __sync_lock_release(&segments_ctl.in_use[num]);
    sem_post(&segments_ctl.semaphore);
}
