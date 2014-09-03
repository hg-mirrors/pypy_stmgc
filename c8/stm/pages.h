
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

static void pages_initialize_private(uintptr_t pagenum, uintptr_t count);


static inline bool is_private_page(long segnum, uintptr_t pagenum)
{
    assert(pagenum >= PAGE_FLAG_START);
    uint64_t bitmask = 1UL << segnum;
    return (pages_privatized[pagenum - PAGE_FLAG_START].by_segment & bitmask);
}
