
enum /* flag_page_private */ {
    /* The page is not in use.  Assume that each segment sees its own copy. */
    FREE_PAGE=0,

    /* The page is shared by all segments.  Each segment sees the same
       physical page (the one that is within the segment 0 mmap address). */
    SHARED_PAGE,

    /* Page is private for each segment.  If we obtain this value outside
       a mutex_pages_lock(), there might be a race: the value can say
       PRIVATE_PAGE before the page is really un-shared. */
    PRIVATE_PAGE,
};

static uint8_t flag_page_private[NB_PAGES];

static void _pages_privatize(uintptr_t pagenum, uintptr_t count, bool full);
static void pages_initialize_shared(uintptr_t pagenum, uintptr_t count);
//static void pages_make_shared_again(uintptr_t pagenum, uintptr_t count);

static void mutex_pages_lock(void);
static void mutex_pages_unlock(void);

inline static void pages_privatize(uintptr_t pagenum, uintptr_t count,
                                   bool full) {
    mutex_pages_lock();
    while (flag_page_private[pagenum] == PRIVATE_PAGE) {
        if (!--count) {
            mutex_pages_unlock();
            return;
        }
        pagenum++;
    }
    mutex_pages_unlock();
    _pages_privatize(pagenum, count, full);
}

//static bool is_fully_in_shared_pages(object_t *obj);  -- not needed?
