
/* Granularity when grabbing more unused pages: take 50 at a time */
#define GCPAGE_NUM_PAGES   50

//static char *uninitialized_page_start;   /* within segment 0 */
//static char *uninitialized_page_stop;

static void setup_gcpage(void);
static void teardown_gcpage(void);
static void setup_N_pages(char *pages_addr, uint64_t num);
static stm_char *allocate_outside_nursery_large(uint64_t size);
