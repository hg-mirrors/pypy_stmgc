
static void setup_detach(void);
static bool fetch_detached_transaction(void);
static void commit_own_inevitable_detached_transaction(stm_thread_local_t *tl);
