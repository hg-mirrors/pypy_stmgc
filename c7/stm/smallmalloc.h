
/* Outside the nursery, we are taking from the highest addresses
   complete pages, one at a time, which uniformly contain objects of
   size "8 * N" for some N in range(2, GC_N_SMALL_REQUESTS).  We are
   taking from the lowest addresses "large" objects, which are at least
   288 bytes long, allocated by largemalloc.c.  The limit is the same
   as used in PyPy's default GC.
*/

#define GC_N_SMALL_REQUESTS    36


struct small_free_loc_s {
    struct small_free_loc_s *next;
};

struct small_page_list_s {
    /* A chained list of locations within the same page which are
       free. */
    struct small_free_loc_s header;

    /* A chained list of all small pages containing objects of
       a given small size, and that have at least one free object. */
    struct small_page_list_s *nextpage;

    /* This structure is only two words, so it always fits inside one
       free slot inside the page. */
};


/* For every size from 16 bytes to 8*(GC_N_SMALL_REQUESTS-1), this is
   a list of pages that contain objects of that size and have at least
   one free location.  Additionally, the item 0 in the following list
   is a chained list of fully-free pages (which can be reused for a
   different size than the one they originally contained).
*/
static struct small_page_list_s *small_page_lists[GC_N_SMALL_REQUESTS];

#define free_uniform_pages   (small_page_lists[0])


/* For is_small_uniform(). */
static uintptr_t first_small_uniform_loc = (uintptr_t) -1;


/* This is a definition for 'STM_PSEGMENT->small_malloc_data'.  Each
   segment grabs one page at a time from the global list, and then
   requests for data are answered locally.
*/
struct small_malloc_data_s {
    struct small_free_loc_s *loc_free[GC_N_SMALL_REQUESTS];
};


/* Functions
 */
static inline char *allocate_outside_nursery_small(uint64_t size)
     __attribute__((always_inline));

static char *_allocate_small_slowpath(uint64_t size);
static void teardown_smallmalloc(void);

static inline bool is_small_uniform(object_t *obj) {
    return ((uintptr_t)obj) >= first_small_uniform_loc;
}
