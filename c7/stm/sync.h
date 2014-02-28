

static void setup_sync(void);
static void teardown_sync(void);

/* all synchronization is done via a mutex and a condition variable */
static void mutex_lock(void);
static void mutex_unlock(void);
static void cond_wait(void);
static void cond_broadcast(void);
#ifndef NDEBUG
static bool _has_mutex(void);
#endif
static void set_gs_register(char *value);

/* acquire and release one of the segments for running the given thread
   (must have the mutex acquired!) */
static bool acquire_thread_segment(stm_thread_local_t *tl);
static void release_thread_segment(stm_thread_local_t *tl);
static void wait_for_end_of_inevitable_transaction(bool can_abort);

/* see the source for an exact description */
static void wait_for_other_safe_points(void);
static bool try_wait_for_other_safe_points(void);
static void collectable_safe_point(void);
