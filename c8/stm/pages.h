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

#define USE_REMAP_FILE_PAGES

struct page_shared_s {
#if NB_SEGMENTS <= 8
    uint8_t by_segment;
#elif NB_SEGMENTS <= 16
    uint16_t by_segment;
#elif NB_SEGMENTS <= 32
    uint32_t by_segment;
#elif NB_SEGMENTS <= 64
    uint64_t by_segment;
#else
#   error "NB_SEGMENTS > 64 not supported right now"
#endif
};

static struct page_shared_s pages_privatized[PAGE_FLAG_END - PAGE_FLAG_START];
static struct page_shared_s pages_readable[PAGE_FLAG_END - PAGE_FLAG_START];

static void pages_initialize_shared(uintptr_t pagenum, uintptr_t count);
static void page_privatize(uintptr_t pagenum);
static void pages_set_protection(int segnum, uintptr_t pagenum,
                                 uintptr_t count, int prot);


static inline uintptr_t get_virt_page_of(long segnum, uintptr_t pagenum)
{
    /* logical page -> virtual page */
    return (uintptr_t)get_segment_base(segnum) / 4096UL + pagenum;
}

static inline bool is_shared_log_page(uintptr_t pagenum)
{
    assert(pagenum >= PAGE_FLAG_START);
    return pages_privatized[pagenum - PAGE_FLAG_START].by_segment == 0;
}

static inline void set_page_private_in(long segnum, uintptr_t pagenum)
{
    uint64_t bitmask = 1UL << segnum;
    volatile struct page_shared_s *ps = (volatile struct page_shared_s *)
        &pages_privatized[pagenum - PAGE_FLAG_START];
    assert(!(ps->by_segment & bitmask));
    ps->by_segment |= bitmask;
}

static inline bool is_private_log_page_in(long segnum, uintptr_t pagenum)
{
    assert(pagenum >= PAGE_FLAG_START);
    uint64_t bitmask = 1UL << segnum;
    return (pages_privatized[pagenum - PAGE_FLAG_START].by_segment & bitmask);
}

static inline bool is_readable_log_page_in(long segnum, uintptr_t pagenum)
{
    assert(pagenum >= PAGE_FLAG_START);
    uint64_t bitmask = 1UL << segnum;
    return (pages_readable[pagenum - PAGE_FLAG_START].by_segment & bitmask);
}
