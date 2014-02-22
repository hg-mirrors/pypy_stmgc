
/* Outside the nursery, we are taking from the highest addresses
   complete pages, one at a time, which uniformly contain objects
   of size "8 * N" for some N in range(2, GC_N_SMALL_REQUESTS).  We
   are taking from the lowest addresses "large" objects, which are
   guaranteed to be at least 256 bytes long (actually 288),
   allocated by largemalloc.c.
*/

#define GC_N_SMALL_REQUESTS    36
#define GC_MEDIUM_REQUEST      (GC_N_SMALL_REQUESTS * 8)


static char *uninitialized_page_start;   /* within segment 0 */
static char *uninitialized_page_stop;


struct small_alloc_s {
    char *next_object;   /* the next address we will return, or NULL */
    char *range_last;    /* if equal to next_object: next_object starts with
                            a next pointer; if greater: last item of a
                            contiguous range of unallocated objs */
};

/* For each small request size, we have three independent chained lists
   of address ranges:

   - 'small_alloc_shared': ranges are within pages that are likely to be
     shared.  We don't know for sure, because pages can be privatized
     by normal run of stm_write().

   - 'small_alloc_sh_old': moved from 'small_alloc_shared' when we're
     looking for a range with the creation_marker set; this collects
     the unsuitable ranges, i.e. the ones with already at least one
     object and no creation marker.

   - 'small_alloc_privtz': ranges are within pages that are privatized.
*/
static struct small_alloc_s small_alloc_shared[GC_N_SMALL_REQUESTS];
static struct small_alloc_s small_alloc_sh_old[GC_N_SMALL_REQUESTS];
static struct small_alloc_s small_alloc_privtz[GC_N_SMALL_REQUESTS];
static char *free_uniform_pages;

static void setup_gcpage(void);
static void teardown_gcpage(void);
//static void check_gcpage_still_shared(void);
static char *allocate_outside_nursery_large(uint64_t size);


static char *_allocate_small_slowpath(
        struct small_alloc_s small_alloc[], uint64_t size);

static inline char *allocate_outside_nursery_small(
        struct small_alloc_s small_alloc[], uint64_t size)
{
    uint64_t index = size / 8;
    OPT_ASSERT(2 <= index);
    OPT_ASSERT(index < GC_N_SMALL_REQUESTS);

    char *result = small_alloc[index].next_object;
    if (result == NULL)
        return _allocate_small_slowpath(small_alloc, size);

    char *following;
    if (small_alloc[index].range_last == result) {
        following = ((char **)result)[0];
        small_alloc[index].range_last = ((char **)result)[1];
    }
    else {
        following = result + size;
    }
    small_alloc[index].next_object = following;

    return result;
}
