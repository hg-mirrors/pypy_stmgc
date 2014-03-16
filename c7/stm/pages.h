
/* This handles pages of objects outside the nursery.  Every page
   has a "shared copy" and zero or more "private copies".

   The shared copy of a page is stored in the mmap at the file offset
   corresponding to the segment 0 offset (with all other segments
   remapping to the segment 0 offset).  Private copies for segment N are
   made in the offset from segment N (for 1 <= N <= NB_SEGMENTS),
   picking file offsets that are simply the next free ones.  Each
   segment maintains a tree 'private_page_mapping', which maps shared
   pages to private copies.

   A major collection looks for pages that are no-longer-used private
   copies, and discard them, remapping the address to the shared page.
   The pages thus freed are recorded into a free list, and can be reused
   as the private copies of the following (unrelated) pages.

   Note that this page manipulation logic uses remap_file_pages() to
   fully hide its execution cost behind the CPU's memory management unit.
   It should not be confused with the logic of tracking which objects
   are old-and-committed, old-but-modified, overflow objects, and so on
   (which works at the object granularity, not the page granularity).
*/

static void page_privatize(uintptr_t pagenum);
static void pages_initialize_shared(uintptr_t pagenum, uintptr_t count);

static void mutex_pages_lock(void);
static void mutex_pages_unlock(void);
static uint64_t increment_total_allocated(ssize_t add_or_remove);
static bool is_major_collection_requested(void);
static void force_major_collection_request(void);
static void reset_major_collection_requested(void);

static inline bool is_private_page(long segnum, uintptr_t pagenum)
{
    return tree_contains(get_priv_segment(segnum)->private_page_mapping,
                         pagenum);
}
