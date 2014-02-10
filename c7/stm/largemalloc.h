
/* all addresses passed to this interface should be "char *" pointers
   in the segment 0. */
static void largemalloc_init_arena(char *data_start, size_t data_size);
static int largemalloc_resize_arena(size_t new_size) __attribute__((unused));

/* large_malloc() and large_free() are not thread-safe.  This is
   due to the fact that they should be mostly called during minor or
   major collections, which have their own synchronization mecanisms. */
static char *large_malloc(size_t request_size);
static void large_free(char *data) __attribute__((unused));
