
/* This handles pages of objects outside the nursery.  Every page
   has a "shared copy" and zero or more "private copies".

   The shared copy of a page is stored in the mmap at the file offset
   corresponding to the segment 0 offset (with all other segments
   remapping to the segment 0 offset).  Private copies are made in the
   offset from segment 1 (and if full, more segments afterwards),
   picking file offsets that are simply the next free ones.  Each
   segment maintains a tree 'private_page_mapping', which maps shared
   pages to private copies.

   A major collection looks for pages that are no-longer-used private
   copies, and discard them, remapping the address to the shared page.
   The pages thus freed are recorded into a free list, and can be reused
   as the private copies of the following (unrelated) pages.

   Note that this page manipulation logic is independent from actually
   tracking which objects are uncommitted, which occurs at the level of
   segment-relative offsets; and propagating changes during commit,
   which is done by copying objects (not pages) to the same offset
   relative to a different segment.
*/

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
