static void setup_mmap(char *reason);
static void close_fd_mmap(int map_fd);
static void setup_protection_settings(void);
static pthread_t *_get_cpth(stm_thread_local_t *);

#ifndef NDEBUG
static __thread long _stm_segfault_expected = false;
#define DEBUG_EXPECT_SEGFAULT(v) do {_stm_segfault_expected = (v);} while (0)
#else
#define DEBUG_EXPECT_SEGFAULT(v) {}
#endif
