

/* when to trigger a CLE collection */
#ifndef STM_TESTS
#define CLE_COLLECT_BOUND (30*1024*1024) /* 30 MiB */
#else
#define CLE_COLLECT_BOUND 0 /* ASAP! */
#endif


/* Commit Log things */
struct stm_undo_s {
    object_t *object;   /* the object that is modified */
    char *backup;       /* some backup data (a slice of the original obj) */
    uint64_t slice;     /* location and size of this slice (cannot cross
                           pages).  The size is in the lower 2 bytes, and
                           the offset in the remaining 6 bytes. */
};
#define SLICE_OFFSET(slice)  ((slice) >> 16)
#define SLICE_SIZE(slice)    ((int)((slice) & 0xFFFF))
#define NEW_SLICE(offset, size) (((uint64_t)(offset)) << 16 | (size))



/* The model is: we have a global chained list, from 'commit_log_root',
   of 'struct stm_commit_log_entry_s' entries.  Every one is fully
   read-only apart from the 'next' field.  Every one stands for one
   commit that occurred.  It lists the old objects that were modified
   in this commit, and their attached "undo logs" --- that is, the
   data from 'written[n].backup' is the content of (slices of) the
   object as they were *before* that commit occurred.
*/
#define INEV_RUNNING ((void*)-1)
struct stm_commit_log_entry_s {
    struct stm_commit_log_entry_s *volatile next;
    int segment_num;
    uint64_t rev_num;
    size_t written_count;
    struct stm_undo_s written[];
};
static struct stm_commit_log_entry_s commit_log_root;


static bool is_cle_collection_requested(void);

static char *malloc_bk(size_t bk_size);
static void free_bk(struct stm_undo_s *undo);
static struct stm_commit_log_entry_s *malloc_cle(long entries);
static void free_cle(struct stm_commit_log_entry_s *e);

void _dbg_print_commit_log();

#ifdef STM_TESTS
uint64_t _stm_cle_allocated(void);
#endif
