

static void setup_sync(void);
static void teardown_sync(void);

/* all synchronization is done via a mutex and a few condition variables */
enum cond_type_e {
    C_RELEASE_THREAD_SEGMENT,
    C_SAFE_POINT,
    C_RESUME,
    C_INEVITABLE_DONE,
    _C_TOTAL
};
static void mutex_lock(void);
static void mutex_unlock(void);
static void cond_wait(enum cond_type_e);
static void cond_broadcast(enum cond_type_e);
static void cond_signal(enum cond_type_e);
#ifndef NDEBUG
static bool _has_mutex(void);
#endif

/* acquire and release one of the segments for running the given thread
   (must have the mutex acquired!) */
static bool acquire_thread_segment(stm_thread_local_t *tl);
static void release_thread_segment(stm_thread_local_t *tl);
static void wait_for_end_of_inevitable_transaction(bool can_abort);

/* see the source for an exact description */
static void wait_for_other_safe_points(int requested_safe_point_kind);
static void collectable_safe_point(void);
