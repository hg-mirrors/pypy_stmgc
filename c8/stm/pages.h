/*
  We have logical pages: one %gs relative pointer can point in some
      logical page
  We have virtual pages: one virtual address can point in some
      virtual page. We have NB_SEGMENTS virtual pages per logical page.
  We have file pages: they correspond mostly to physical memory pages
      used for mmap/remap_file_pages

  A logical page is SHARED iff all NB_SEGMENTS virtual pages point to
  one file page, and thus to the same logical page.

  A logical page becomes PRIVATE if one virtual page still maps to the
  original file page, and all others turn read protected.
   -> only one can modify it.

  A logical page can also be "PRIVATE IN A SEGMENT", referring to
  the virtual page of the segment having its own file page backing.
  It also implies the logical page is not read protected.
*/

#define PAGE_FLAG_START   END_NURSERY_PAGE
#define PAGE_FLAG_END     NB_PAGES
/* == NB_SHARED_PAGES */


struct page_shared_s {
#if NB_SEGMENTS <= 4
    uint8_t by_segment;
#elif NB_SEGMENTS <= 8
    uint16_t by_segment;
#elif NB_SEGMENTS <= 16
    uint32_t by_segment;
#elif NB_SEGMENTS <= 32
    uint64_t by_segment;
#else
#   error "NB_SEGMENTS > 32 not supported right now"
#endif
};

enum {
    PAGE_SHARED = 0,
    PAGE_PRIVATE,
    PAGE_NO_ACCESS,
};

static struct page_shared_s pages_status[NB_SHARED_PAGES];

static void pages_initialize_shared_for(long segnum, uintptr_t pagenum, uintptr_t count);
static void page_privatize_in(int segnum, uintptr_t pagenum);
static void memcpy_to_accessible_pages(int dst_segnum, object_t *dst_obj, char *src, size_t len);




static inline uintptr_t get_virt_page_of(long segnum, uintptr_t pagenum)
{
    /* logical page -> virtual page */
    return (uintptr_t)get_segment_base(segnum) / 4096UL + pagenum;
}

static inline uintptr_t get_file_page_of(uintptr_t pagenum)
{
    /* logical page -> file page */
    return pagenum - PAGE_FLAG_START;
}


static inline uint8_t get_page_status_in(long segnum, uintptr_t pagenum)
{
    int seg_shift = segnum * 2;
    uint64_t bitmask = 3UL << seg_shift;
    volatile struct page_shared_s *ps = (volatile struct page_shared_s *)
        &pages_status[get_file_page_of(pagenum)];

    return ((ps->by_segment & bitmask) >> seg_shift) & 3;
}

static inline void set_page_status_in(long segnum, uintptr_t pagenum, uint8_t status)
{
    OPT_ASSERT((status & 3) == status);

    int seg_shift = segnum * 2;
    volatile struct page_shared_s *ps = (volatile struct page_shared_s *)
        &pages_status[get_file_page_of(pagenum)];

    assert(status != get_page_status_in(segnum, pagenum));
    ps->by_segment |= status << seg_shift;
}
