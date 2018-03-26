static void setup_sync(void);
static void teardown_sync(void);

enum cond_type_e {
    C_AT_SAFE_POINT,
    C_REQUEST_REMOVED,
    C_SEGMENT_FREE,
    C_SEGMENT_FREE_OR_SAFE_POINT_REQ,
    C_QUEUE_OLD_ENTRIES,
    C_QUEUE_FINISHED_MORE_TASKS,
    _C_TOTAL
};

static void s_mutex_lock(void);
static void s_mutex_unlock(void);
static void cond_wait(enum cond_type_e);
static void cond_signal(enum cond_type_e);
static void cond_broadcast(enum cond_type_e);
#ifndef NDEBUG
static bool _has_mutex(void);
#endif
static void set_gs_register(char *value);
static void ensure_gs_register(long segnum);


/* acquire and release one of the segments for running the given thread
   (must have the mutex acquired!) */
static void acquire_thread_segment(stm_thread_local_t *tl);
static void release_thread_segment(stm_thread_local_t *tl);
static void soon_finished_or_inevitable_thread_segment(void);
static bool any_soon_finished_or_inevitable_thread_segment(void);
static struct stm_priv_segment_info_s* get_inevitable_thread_segment(void);

enum sync_type_e {
    STOP_OTHERS_UNTIL_MUTEX_UNLOCK,
    STOP_OTHERS_AND_BECOME_GLOBALLY_UNIQUE,
};
static void synchronize_all_threads(enum sync_type_e sync_type);
static void committed_globally_unique_transaction(void);

static bool pause_signalled, globally_unique_transaction;
static void enter_safe_point_if_requested(void);

#define safe_point_requested()  (STM_SEGMENT->nursery_end != NURSERY_END)
