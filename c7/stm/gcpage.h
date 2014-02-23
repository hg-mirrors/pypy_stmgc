
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

static struct small_alloc_s small_alloc[GC_N_SMALL_REQUESTS];
static char *free_uniform_pages;

static void setup_gcpage(void);
static void teardown_gcpage(void);
//static char *allocate_outside_nursery_large(uint64_t size);


static char *_allocate_small_slowpath(uint64_t size);

static inline char *allocate_outside_nursery_small(uint64_t size)
{
    uint64_t index = size / 8;
    OPT_ASSERT(2 <= index);
    OPT_ASSERT(index < GC_N_SMALL_REQUESTS);

    char *result = small_alloc[index].next_object;
    if (result == NULL)
        return _allocate_small_slowpath(size);

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
