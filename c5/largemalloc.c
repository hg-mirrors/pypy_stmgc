/* This contains a lot of inspiration from malloc() in the GNU C Library.
   More precisely, this is (a subset of) the part that handles large
   blocks, which in our case means at least 288 bytes.
*/

#include <stdlib.h>
#include <stdio.h>
#include <stddef.h>
#include <assert.h>


#define MMAP_LIMIT    (1280*1024)

#define largebin_index(sz)                                      \
    (((sz) < (48 <<  6)) ?      ((sz) >>  6):  /*  0 - 47 */    \
     ((sz) < (24 <<  9)) ? 42 + ((sz) >>  9):  /* 48 - 65 */    \
     ((sz) < (12 << 12)) ? 63 + ((sz) >> 12):  /* 66 - 74 */    \
     ((sz) < (6  << 15)) ? 74 + ((sz) >> 15):  /* 75 - 79 */    \
     ((sz) < (3  << 18)) ? 80 + ((sz) >> 18):  /* 80 - 82 */    \
                           83)
#define N_BINS             84
#define LAST_BIN_INDEX(sz) ((sz) >= (3 << 18))

typedef struct dlist_s {
    struct dlist_s *next;   /* a doubly-linked list */
    struct dlist_s *prev;
} dlist_t;

typedef struct malloc_chunk {
    size_t prev_size;     /* - if the previous chunk is free: size of its data
                             - otherwise, if this chunk is free: 1
                             - otherwise, 0. */
    size_t size;          /* size of the data in this chunk,
                             plus optionally the FLAG_UNSORTED */

    dlist_t d;            /* if free: a doubly-linked list */
                          /* if not free: the user data starts here */

    /* The chunk has a total size of 'size'.  It is immediately followed
       in memory by another chunk.  This list ends with the last "chunk"
       being actually only one word long, 'size_t prev_size'.  Both this
       last chunk and the theoretical chunk before the first one are
       considered "not free". */
} mchunk_t;

#define FLAG_UNSORTED        1
#define THIS_CHUNK_FREE      1
#define BOTH_CHUNKS_USED     0
#define CHUNK_HEADER_SIZE    offsetof(struct malloc_chunk, d)

#define chunk_at_offset(p, ofs)  ((mchunk_t *)(((char *)(p)) + (ofs)))
#define data2chunk(p)            chunk_at_offset(p, -CHUNK_HEADER_SIZE)
#define next_chunk(p)         chunk_at_offset(p, CHUNK_HEADER_SIZE + (p)->size)


/* The free chunks are stored in "bins".  Each bin is a doubly-linked
   list of chunks.  There are 84 bins, with largebin_index() giving the
   correspondence between sizes are bin indices.

   Each free chunk is preceeded in memory by a non-free chunk (or no
   chunk at all).  Each free chunk is followed in memory by a non-free
   chunk (or no chunk at all).  Chunks are consolidated with their
   neighbors to ensure this.

   In each bin's doubly-linked list, chunks are sorted by their size in
   decreasing order (if you start from 'd.next').  At the end of this
   list are some unsorted chunks (with FLAG_UNSORTED).  All unsorted
   chunks are after all sorted chunks.
*/

static dlist_t largebins[N_BINS] = {

#define INIT(num)   { largebins + num, largebins + num }
    INIT(0),  INIT(1),  INIT(2),  INIT(3),  INIT(4),
    INIT(5),  INIT(6),  INIT(7),  INIT(8),  INIT(9),
    INIT(10), INIT(11), INIT(12), INIT(13), INIT(14),
    INIT(15), INIT(16), INIT(17), INIT(18), INIT(19),
    INIT(20), INIT(21), INIT(22), INIT(23), INIT(24),
    INIT(25), INIT(26), INIT(27), INIT(28), INIT(29),
    INIT(30), INIT(31), INIT(32), INIT(33), INIT(34),
    INIT(35), INIT(36), INIT(37), INIT(38), INIT(39),
    INIT(40), INIT(41), INIT(42), INIT(43), INIT(44),
    INIT(45), INIT(46), INIT(47), INIT(48), INIT(49),
    INIT(50), INIT(51), INIT(52), INIT(53), INIT(54),
    INIT(55), INIT(56), INIT(57), INIT(58), INIT(59),
    INIT(60), INIT(61), INIT(62), INIT(63), INIT(64),
    INIT(65), INIT(66), INIT(67), INIT(68), INIT(69),
    INIT(70), INIT(71), INIT(72), INIT(73), INIT(74),
    INIT(75), INIT(76), INIT(77), INIT(78), INIT(79),
    INIT(80), INIT(81), INIT(82), INIT(83) };
#undef INIT


static char *allocate_more(size_t request_size);

static void insert_unsorted(mchunk_t *new)
{
    size_t index = LAST_BIN_INDEX(new->size) ? N_BINS - 1
                                             : largebin_index(new->size);
    new->d.next = &largebins[index];
    new->d.prev = largebins[index].prev;
    new->d.prev->next = &new->d;
    largebins[index].prev = &new->d;
    new->size |= FLAG_UNSORTED;
}

static int compare_chunks(const void *vchunk1, const void *vchunk2)
{
    /* sort by size */
    const mchunk_t *chunk1 = (const mchunk_t *)vchunk1;
    const mchunk_t *chunk2 = (const mchunk_t *)vchunk2;
    if (chunk1->size < chunk2->size)
        return -1;
    if (chunk1->size == chunk2->size)
        return 0;
    else
        return +1;
}

static void really_sort_bin(size_t index)
{
    dlist_t *unsorted = largebins[index].prev;
    dlist_t *end = &largebins[index];
    dlist_t *scan = unsorted->prev;
    size_t count = 1;
    while (scan != end && (data2chunk(scan)->size & FLAG_UNSORTED)) {
        scan = scan->prev;
        ++count;
    }
    end->prev = scan;
    scan->next = end;

    mchunk_t *chunks[count];
    size_t i;
    for (i = 0; i < count; i++) {
        chunks[i] = data2chunk(unsorted);
        unsorted = unsorted->prev;
    }
    assert(unsorted == scan);
    qsort(chunks, count, sizeof(mchunk_t *), compare_chunks);

    --count;
    chunks[count]->size &= ~FLAG_UNSORTED;
    size_t search_size = chunks[count]->size;
    dlist_t *head = largebins[index].next;

    while (1) {
        if (head == end || search_size >= data2chunk(head)->size) {
            /* insert 'chunks[count]' here, before the current head */
            head->prev->next = &chunks[count]->d;
            chunks[count]->d.prev = head->prev;
            head->prev = &chunks[count]->d;
            chunks[count]->d.next = head;
            if (count == 0)
                break;    /* all done */
            --count;
            chunks[count]->size &= ~FLAG_UNSORTED;
            search_size = chunks[count]->size;
        }
        else {
            head = head->next;
        }
    }
}

static void sort_bin(size_t index)
{
    dlist_t *last = largebins[index].prev;
    if (last != &largebins[index] && (data2chunk(last)->size & FLAG_UNSORTED))
        really_sort_bin(index);
}

char *stm_large_malloc(size_t request_size)
{
    /* 'request_size' should already be a multiple of the word size here */
    assert((request_size & (sizeof(char *)-1)) == 0);

    size_t index = largebin_index(request_size);
    sort_bin(index);

    /* scan through the chunks of current bin in reverse order
       to find the smallest that fits. */
    dlist_t *scan = largebins[index].prev;
    dlist_t *end = &largebins[index];
    mchunk_t *mscan;
    while (scan != end) {
        mscan = data2chunk(scan);
        assert(mscan->prev_size == THIS_CHUNK_FREE);
        assert(next_chunk(mscan)->prev_size == mscan->size);

        if (mscan->size >= request_size)
            goto found;
        scan = mscan->d.prev;
    }

    /* search now through all higher bins.  We only need to take the
       smallest item of the first non-empty bin, as it will be large
       enough.  xxx use a bitmap to speed this up */
    while (++index < N_BINS) {
        sort_bin(index);
        scan = largebins[index].prev;
        end = &largebins[index];
        if (scan != end) {
            mscan = data2chunk(scan);
            assert(mscan->size >= request_size);
            goto found;
        }
    }

    /* not enough free memory.  We need to allocate more. */
    return allocate_more(request_size);

 found:
    /* unlink mscan from the doubly-linked list */
    mscan->d.next->prev = mscan->d.prev;
    mscan->d.prev->next = mscan->d.next;

    size_t remaining_size = mscan->size - request_size;
    if (remaining_size < sizeof(struct malloc_chunk)) {
        next_chunk(mscan)->prev_size = BOTH_CHUNKS_USED;
    }
    else {
        /* only part of the chunk is being used; reduce the size
           of 'mscan' down to 'request_size', and create a new
           chunk of the 'remaining_size' afterwards */
        mchunk_t *new = chunk_at_offset(mscan, CHUNK_HEADER_SIZE +
                                               request_size);
        new->prev_size = THIS_CHUNK_FREE;
        remaining_size -= CHUNK_HEADER_SIZE;
        new->size = remaining_size;
        next_chunk(new)->prev_size = remaining_size;
        insert_unsorted(new);
        mscan->size = request_size;
    }
    mscan->prev_size = BOTH_CHUNKS_USED;
    return (char *)&mscan->d;
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
    big_chunk->size = big_size - CHUNK_HEADER_SIZE - sizeof(size_t);

    assert((char *)&next_chunk(big_chunk)->prev_size ==
           ((char *)big_chunk) + big_size - sizeof(size_t));
    next_chunk(big_chunk)->prev_size = big_chunk->size;

    insert_unsorted(big_chunk);

    return stm_large_malloc(request_size);
}

void stm_large_free(char *data)
{
    mchunk_t *chunk = data2chunk(data);
    assert((chunk->size & (sizeof(char *) - 1)) == 0);
    assert(chunk->prev_size != THIS_CHUNK_FREE);

    /* try to merge with the following chunk in memory */
    size_t msize = chunk->size + CHUNK_HEADER_SIZE;
    mchunk_t *mscan = chunk_at_offset(chunk, msize);

    if (mscan->prev_size == BOTH_CHUNKS_USED) {
        assert((mscan->size & (sizeof(char *) - 1)) == 0);
        mscan->prev_size = chunk->size;
    }
    else {
        mscan->size &= ~FLAG_UNSORTED;
        size_t fsize = mscan->size;
        mchunk_t *fscan = chunk_at_offset(mscan, fsize + CHUNK_HEADER_SIZE);

        /* unlink the following chunk */
        mscan->d.next->prev = mscan->d.prev;
        mscan->d.prev->next = mscan->d.next;
        assert(mscan->prev_size = (size_t)-1);
        assert(mscan->size = (size_t)-1);

        /* merge the two chunks */
        assert(fsize == fscan->prev_size);
        fsize += msize;
        fscan->prev_size = fsize;
        chunk->size = fsize;
    }

    /* try to merge with the previous chunk in memory */
    if (chunk->prev_size == BOTH_CHUNKS_USED) {
        chunk->prev_size = THIS_CHUNK_FREE;
    }
    else {
        assert((chunk->prev_size & (sizeof(char *) - 1)) == 0);

        /* get at the previous chunk */
        msize = chunk->prev_size + CHUNK_HEADER_SIZE;
        mscan = chunk_at_offset(chunk, -msize);
        assert(mscan->prev_size == THIS_CHUNK_FREE);
        assert((mscan->size & ~FLAG_UNSORTED) == chunk->prev_size);

        /* unlink the previous chunk */
        mscan->d.next->prev = mscan->d.prev;
        mscan->d.prev->next = mscan->d.next;

        /* merge the two chunks */
        mscan->size = msize + chunk->size;
        next_chunk(mscan)->prev_size = mscan->size;

        assert(chunk->prev_size = (size_t)-1);
        assert(chunk->size = (size_t)-1);
        chunk = mscan;
    }

    insert_unsorted(chunk);
}
