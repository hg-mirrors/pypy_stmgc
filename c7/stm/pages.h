
enum {
    /* The page is not in use.  Assume that each segment sees its own copy. */
    FREE_PAGE=0,

    /* The page is shared by all segments.  Each segment sees the same
       physical page (the one that is within the segment 0 mmap address). */
    SHARED_PAGE,

    /* Page being in the process of privatization */
    REMAPPING_PAGE,

    /* Page is private for each segment. */
    PRIVATE_PAGE,

};      /* used for flag_page_private */


static uint8_t flag_page_private[NB_PAGES];

static void _pages_privatize(uintptr_t pagenum, uintptr_t count);
static void pages_initialize_shared(uintptr_t pagenum, uintptr_t count);

inline static void pages_privatize(uintptr_t pagenum, uintptr_t count) {
    while (flag_page_private[pagenum] == PRIVATE_PAGE) {
        if (!--count)
            return;
        pagenum++;
    }
    _pages_privatize(pagenum, count);
}

static void set_creation_markers(stm_char *p, uint64_t size, int newvalue);
static uint8_t get_single_creation_marker(stm_char *p) __attribute__((unused));
static void set_single_creation_marker(stm_char *p, int newvalue);
static void reset_all_creation_markers(void);
static void reset_all_creation_markers_and_push_created_data(void);
