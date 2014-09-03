

static char *uninitialized_page_start;   /* within segment 0 */
static char *uninitialized_page_stop;

static void setup_gcpage(void);
static void teardown_gcpage(void);
static char *allocate_outside_nursery_large(uint64_t size);
