
static char *uninitialized_page_start;   /* within segment 0 */
static char *uninitialized_page_stop;

static char *allocate_outside_nursery(uint64_t size);
