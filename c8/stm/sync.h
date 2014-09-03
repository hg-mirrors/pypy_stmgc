static void setup_sync(void);
static void teardown_sync(void);


static void s_mutex_lock(void);
static void s_mutex_unlock(void);
#ifndef NDEBUG
static bool _has_mutex(void);
#endif
static void set_gs_register(char *value);


/* acquire and release one of the segments for running the given thread
   (must have the mutex acquired!) */
static bool acquire_thread_segment(stm_thread_local_t *tl);
static void release_thread_segment(stm_thread_local_t *tl);
