
/* For every page, 'num_segments_sharing_page' stores a number that
   counts the number of segments that share the page.  If 0, the page is
   not used so far.

   When the page is first initialized, 'num_segments_sharing_page' is
   set to NB_SEGMENTS.  When later a segment wants a private copy, it
   looks first in its own 'private_page_mapping' tree, which maps shared
   pages to private copies.  If not found, then it proceeds like this:

   If 'num_segments_sharing_page' is greater than 1, then it is
   decremented and a private copy of the page is made.

   If 'num_segments_sharing_page' is equal to 1, then we know we are the
   last segment that sees this "shared" copy, and so it is actually not
   shared with anybody else --- i.e. it is private already.

   The shared copy of a page is stored in the mmap at the file offset
   corresponding to the segment 0 offset (with all other segments
   remapping to the segment 0 offset).  Private copies are made in the
   offset from segment 1 (and if full, more segments afterwards),
   picking file offsets that are simply the next free ones.  This is
   probably good for long-term memory usage: a major collection looks
   for pages that are no-longer-used private copies of some shared page,
   and discard them, remapping the address to the shared page.  The
   pages thus freed are recorded into a free list, and can be reused as
   the private copies of the following (unrelated) pages.
*/
static uint8_t num_segments_sharing_page[NB_PAGES];

static void _pages_privatize(uintptr_t pagenum, uintptr_t count);
static void pages_initialize_shared(uintptr_t pagenum, uintptr_t count);

static void mutex_pages_lock(void);
static void mutex_pages_unlock(void);
static uint64_t increment_total_allocated(ssize_t add_or_remove);
static bool is_major_collection_requested(void);
static void force_major_collection_request(void);
static void reset_major_collection_requested(void);

inline static void pages_privatize(uintptr_t pagenum, uintptr_t count) {
    /* This is written a bit carefully so that a call with a constant
       count == 1 will turn this loop into just one "if". */
    while (flag_page_private[pagenum] == PRIVATE_PAGE) {
        if (!--count) {
            return;
        }
        pagenum++;
    }
    _pages_privatize(pagenum, count);
}
