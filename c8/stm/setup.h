static void setup_mmap(char *reason);
static void setup_protection_settings(void);
static pthread_t *_get_cpth(stm_thread_local_t *);
static void detect_shadowstack_overflow(char *);

#ifndef NDEBUG
static __thread long _stm_segfault_expected = 1;
#define DEBUG_EXPECT_SEGFAULT(v) do {if (v) _stm_segfault_expected++; else _stm_segfault_expected--; assert(_stm_segfault_expected <= 1);} while (0)
#else
#define DEBUG_EXPECT_SEGFAULT(v) {}
#endif
