

static void setup_sync(void);
static void teardown_sync(void);

/* acquire and release one of the segments for running the given thread */
static void acquire_thread_segment(stm_thread_local_t *tl);
static void release_thread_segment(stm_thread_local_t *tl);
