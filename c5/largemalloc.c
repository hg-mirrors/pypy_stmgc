/* This contains a lot of inspiration from malloc() in the GNU C Library.
   More precisely, this is (a subset of) the part that handles large
   blocks, which in our case means at least 288 bytes.
*/


#define MMAP_LIMIT    (1280*1024)

#define largebin_index(sz)                                       \
    ((((sz) >>  6) <= 47) ?      ((sz) >>  6):  /*  0 - 47 */    \
     (((sz) >>  9) <= 23) ? 42 + ((sz) >>  9):  /* 48 - 65 */    \
     (((sz) >> 12) <= 11) ? 63 + ((sz) >> 12):  /* 66 - 74 */    \
     (((sz) >> 15) <=  5) ? 74 + ((sz) >> 15):  /* 75 - 79 */    \
     (((sz) >> 18) <=  2) ? 80 + ((sz) >> 18):  /* 80 - 82 */    \
                            83)
#define N_BINS              84

typedef struct malloc_chunk {
    size_t prev_size;     /* - if the previous chunk is free: its size
                             - otherwise, if this chunk is free: 1
                             - otherwise, 0. */
    size_t size;          /* size of this chunk */

    union {
        char data[1];               /* if not free: start of the user data */
        struct malloc_chunk *next;  /* if free: a doubly-linked list */
    };
    struct malloc_chunk *prev;

    /* The chunk has a total size of 'size'.  It is immediately followed
       in memory by another chunk.  This list ends with the last "chunk"
       being actually only one word long, 'size_t prev_size'.  Both this
       last chunk and the theoretical chunk before the first one are
       considered "not free". */
} *mchunk_t;

#define THIS_CHUNK_FREE      1
#define BOTH_CHUNKS_USED     0
#define CHUNK_HEADER_SIZE    offsetof(struct malloc_chunk, data)
#define MINSIZE              sizeof(struct malloc_chunk)

#define chunk_at_offset(p, ofs)  ((mchunk_t *)(((char *)(p)) + ofs))
#define next_chunk(p)            chunk_at_offset(p, (p)->size)


/* The free chunks are stored in "bins".  Each bin is a doubly-linked
   list of chunks.  There are 84 bins, with largebin_index() giving the
   correspondence between sizes are bin indices.

   Each free chunk is preceeded in memory by a non-free chunk (or no
   chunk at all).  Each free chunk is followed in memory by a non-free
   chunk (or no chunk at all).  Chunks are consolidated with their
   neighbors to ensure this.

   In each bin's doubly-linked list, chunks are sorted by their size in
   decreasing order .  Among chunks of equal size, they are ordered with
   the most recently freed first, and we take them from the back.  This
   results in FIFO order, which is better to give each block a while to
   rest in the list and be consolidated into potentially larger blocks.
*/

static struct { mchunk_t *head, mchunk_t *tail; } largebins[N_BINS];


static char *allocate_more(size_t request_size);

static void insert_sort(mchunk_t *new)
{
    size_t index = largebin_index(new->size);
    mchunk_t *head = largebins[index]->head;

    if (head == NULL) {
        assert(largebins[index]->tail == NULL);
        new->prev = NULL;
        new->next = NULL;
        largebins[index]->tail = new;
        largebins[index]->head = new;
        return;
    }
    assert(largebins[index]->tail != NULL);

    size_t new_size = new->size;
    if (new_size >= head->size) {
        new->prev = NULL;
        new->next = head;
        assert(head->prev == NULL);
        head->prev = new;
        largebins[index]->head = new;
        return;
    }
    mchunk_t *search;
    for (search = head; search != NULL; search = search->next) {
        if (new_size >= search->size) {
            new->prev = search->prev;
            new->prev->next = new;
            new->next = search;
            search->prev = new;
            return;
        }
    }
    new->prev = largebins[index]->tail;
    new->prev->next = new;
    new->next = NULL;
    largebins[index]->tail = new;
}

char *stm_large_malloc(size_t request_size)
{
    /* 'request_size' should already be a multiple of the word size here */
    assert((request_size & (sizeof(char *)-1)) == 0);

    size_t chunk_size = request_size + CHUNK_HEADER_SIZE;
    if (chunk_size < request_size) {
        /* 'request_size' is so large that the addition wrapped around */
        fprintf(stderr, "allocation request too large\n");
        abort();
    }

    size_t index = largebin_index(chunk_size);

    /* scan through the chunks of current bin in reverse order
       to find the smallest that fits. */
    mchunk_t *scan = largebins[index]->tail;
    mchunk_t *head = largebins[index]->head;
    while (scan != head) {
        assert(scan->prev_size == THIS_CHUNK_FREE);
        assert(next_chunk(scan)->prev_size == scan->size);

        if (scan->size >= chunk_size) {
            /* found! */
         found:
            if (scan == largebins[index]->tail) {
                largebins[index]->tail = scan->prev;
            }
            else {
                scan->next->prev = scan->prev;
            }

            size_t remaining_size = scan->size - chunk_size;
            if (remaining_size < MINSIZE) {
                next_chunk(scan)->prev_size = BOTH_CHUNKS_USED;
            }
            else {
                /* only part of the chunk is being used; reduce the size
                   of 'scan' down to 'chunk_size', and create a new chunk
                   of the 'remaining_size' afterwards */
                mchunk_t *new = chunk_at_offset(scan, chunk_size);
                new->prev_size = THIS_CHUNK_FREE;
                new->size = remaining_size;
                next_chunk(new)->prev_size = remaining_size;
                insert_sort(new);
                scan->size = chunk_size;
            }
            scan->prev_size = BOTH_CHUNKS_USED;
            return scan->data;
        }
        scan = scan->prev;
    }

    /* search now through all higher bins.  We only need to take the
       smallest item of the first non-empty bin, as it will be large
       enough.  xxx use a bitmap to speed this up */
    while (++index < N_BINS) {
        scan = largebins[index]->tail;
        if (scan != NULL)
            goto found;
    }

    /* not enough free memory.  We need to allocate more. */
    return allocate_more(request_size);
}

static char *allocate_more(size_t request_size)
{
    assert(request_size < MMAP_LIMIT);//XXX

    size_t big_size = MMAP_LIMIT * 8 - 48;
    mchunk_t *big_chunk = (mchunk_t *)malloc(big_size);
    if (!big_chunk) {
        fprintf(stderr, "out of memory!\n");
        abort();
    }

    big_chunk->prev_size = THIS_CHUNK_FREE;
    big_chunk->size = big_size - sizeof(size_t);
    next_chunk(big_chunk)->prev_size = big_chunk->size;
    insert_sort(big_chunk);

    return stm_large_malloc(request_size);
}
