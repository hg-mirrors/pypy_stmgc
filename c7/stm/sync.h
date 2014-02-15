

static void setup_sync(void);
static void teardown_sync(void);

/* all synchronization is done via a mutex and condition variable */
static void mutex_lock(void);
static void mutex_unlock(void);
static void cond_wait(void);
static void cond_broadcast(void);

/* acquire and release one of the segments for running the given thread
   (must have the mutex acquired!) */
static void acquire_thread_segment(stm_thread_local_t *tl);
static void release_thread_segment(stm_thread_local_t *tl);
