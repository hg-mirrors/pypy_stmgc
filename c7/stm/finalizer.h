
struct finalizers_s {
    struct list_s *objects_with_finalizers;
    struct list_s *run_finalizers;
    uintptr_t *running_next;
};

static void deal_with_young_objects_with_finalizers(void);
static void deal_with_old_objects_with_finalizers(void);
static void deal_with_objects_with_finalizers(void);

static void setup_finalizer(void);
static void teardown_finalizer(void);

static void _commit_finalizers(void);
static void _abort_finalizers(void);

#define commit_finalizers()   do {              \
    if (STM_PSEGMENT->finalizers != NULL)       \
        _commit_finalizers();                   \
} while (0)

#define abort_finalizers()   do {               \
    if (STM_PSEGMENT->finalizers != NULL)       \
        _abort_finalizers();                    \
} while (0)


/* regular finalizers (objs from already-committed transactions) */
static struct finalizers_s g_finalizers;

static void _invoke_general_finalizers(stm_thread_local_t *tl);

#define invoke_general_finalizers(tl)    do {   \
    if (g_finalizers.run_finalizers != NULL)    \
        _invoke_general_finalizers(tl);         \
} while (0)

static void execute_finalizers(struct finalizers_s *f);
