#define _GNU_SOURCE
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <asm/prctl.h>
#include <sys/prctl.h>
#include <errno.h>
#include <assert.h>
#include <string.h>

#include "core.h"
#include "pagecopy.h"


/* This only works with clang, and on 64-bit Linux, for now.
   It depends on:

     * the %gs segment prefix

         This a hack using __attribute__((address_space(256))) on
         structs, which makes clang write all pointer dereferences to
         them using the "%gs:" prefix.  This is a rarely-used way to
         shift all memory accesses by some offset stored in the %gs
         special register.  Each thread has its own value in %gs.  Note
         that %fs is used in a similar way by the pthread library to
         offset the thread-local variables; what we need is similar to
         thread-local variables, but in large quantity.

     * remap_file_pages()

         This is a Linux-only system call that allows us to move or
         duplicate "pages" of an mmap.  A page is 4096 bytes.  The same
         page can be viewed at several addresses.  It gives an mmap
         which appears larger than the physical memory that stores it:
         read/writes at one address are identical to read/writes at
         different addresses as well, by going to the same physical
         memory.  This is important in order to share most pages between
         most threads.

   Here is a more detailed presentation.  All the GC-managed memory is
   in one big mmap, divided in N+1 sections: the first section holds the
   status of the latest committed transaction; and the N following
   sections are thread-local and hold each the status of one of the N
   threads.  These thread-local sections are initially remapped with
   remap_file_pages() to correspond to the same memory as the first
   section.

   When the current transaction does a write to an old page, we call
   remap_file_pages() again to unshare the page in question before
   allowing the write to occur.  (This is similar to what occurs after
   fork(), but done explicitly instead of by the OS.)

   Once a page is unshared, it remains unshared until some event occurs
   (probably the next major collection; not implemented yet).

   The memory content in the common (first) section contains objects
   with regular pointers to each other.  Each thread accesses these
   objects using the %gs segment prefix, which is configured to shift
   the view to this thread's thread-local section.

   To clarify terminology, we will call "object page" a page of memory
   from the common (first) section.  The term "pgoff" refers to a page
   index in this common section.  For convenience this number is a
   uint32_t (so the limit is 2**32 object pages, or 16 terabytes).

   Exact layout (example with 2 threads only):

       <---------------%gs is thread 2----------->

       <---%gs is thread 1-->

       +-------------..   +-+-+    +---+       +-+-+    +---+
       | normal progr.    |L|0|    |RM1|       |L|0|    |RM2|
       +-------------..   +-+-+    +---+       +-+-+    +---+
       ^null address

       +--------------------+--------------------+--------------------+
       |  object pages      |  thread-local 1    |  thread-local 2    |
       +--------------------+--------------------+--------------------+

   There are NB_PAGES object pages; so far it is 1 GB of memory.  The
   big mmap (bottom line) is thus allocated as 3 GB of consecutive
   memory, and %gs is set to 1 billion in thread 1 and 2 billion in
   thread 2.

   The constrains on this layout come from the fact that we'd like the
   objects (in the object pages) to look correct: they contain pointers
   to more objects (also in the object pages), or nulls.  This is not
   really necessary (e.g. we could store indexes from some place, rather
   than real pointers) but should help debugging.

   We also allocate 2*N pages at known addresses: the L pages, just
   before the addresses 1GB and 2GB, contain thread-locals and are
   accessed as %gs:(small negative offset).  The 0 pages are reserved
   but marked as not accessible, to crash cleanly on null pointer
   dereferences, done as %gs:(0).

   Finally we have 64 MB of pages written as RM1 and RM2: they are
   thread-local read markers.  They are placed precisely such that,
   for object address "p", the read marker is at %gs:(p/16).  In the
   diagram above RM1 is placed somewhere between the two L-0 blocks,
   but that's not required.

   This is possible by mmaps at fixed addresses, and hopefully still
   gives enough flexibility to let us try several other sets of
   addresses if the first set is busy.  We use here the fact that the
   total address space available is huge.
*/

#define NB_PAGES   (256*1024)   // 1GB
#define NB_THREADS  16
#define MAP_PAGES_FLAGS  (MAP_SHARED|MAP_ANONYMOUS|MAP_NORESERVE)

#define CACHE_LINE_SIZE  128    // conservatively large value to avoid aliasing

#define LARGE_OBJECT_WORDS        36

struct page_header_s {
    /* Every page starts with one such structure */
    uint8_t obj_word_size;    /* size of all objects in this page, in words
                                 in range(2, LARGE_OBJECT_WORDS) */
    _Bool thread_local_copy;
    uint32_t write_log_index_cache;
};

struct write_entry_s {
    uint32_t pgoff;      /* the pgoff of the page that was modified */
    uint64_t bitmask[4]; /* bit N is set if object at 'N*16' was modified */
} __attribute__((packed));

struct write_history_s {
    struct write_history_s *previous_older_transaction;
    uint16_t transaction_version;
    uint32_t nb_updates;
    struct write_entry_s updates[];
};

struct shared_descriptor_s {
    /* There is a single shared descriptor.  This contains global
       variables, but as a structure, in order to control the aliasing
       at the cache line level --- we don't want the following few
       variables to be accidentally in the same cache line. */
    char pad0[CACHE_LINE_SIZE]; uint64_t volatile index_page_never_used;
    char pad2[CACHE_LINE_SIZE]; struct write_history_s *
                                    volatile most_recent_committed_transaction;
    char pad3[CACHE_LINE_SIZE];
};

struct alloc_for_size_s {
    char *next;
    char *end;
};

typedef GCOBJECT struct _thread_local2_s {
    /* All the thread-local variables we need. */
    struct write_history_s *base_page_mapping;
    struct write_history_s *writes_by_this_transaction;
    uint32_t nb_updates_max;
    struct alloc_for_size_s alloc[LARGE_OBJECT_WORDS];
    uint64_t gs_value;
    _thread_local1_t _stm_tl1;  /* space for the macro _STM_TL1 in core.h */
} _thread_local2_t;

#define _STM_TL2   (((_thread_local2_t *)0)[-1])

char *stm_object_pages;
struct shared_descriptor_s stm_shared_descriptor;
volatile int stm_next_thread_index;


/************************************************************/


_Bool _stm_was_read(object_t *object)
{
    return _STM_CRM[((uintptr_t)object) >> 4].c == _STM_TL1.read_marker;
}

_Bool _stm_was_written(object_t *object)
{
    return (object->flags & GCFLAG_WRITE_BARRIER) == 0;
}


struct page_header_s *_stm_reserve_page(void)
{
    /* Grab a free mm page, and map it into the address space.
       Return a pointer to it. */

    // XXX look in some free list first

    /* Return the index'th object page, which is so far never used. */
    uint64_t index = __sync_fetch_and_add(
        &stm_shared_descriptor.index_page_never_used, 1);
    if (index >= NB_PAGES) {
        fprintf(stderr, "Out of mmap'ed memory!\n");
        abort();
    }
    return (struct page_header_s *)(stm_object_pages + index * 4096UL);
}


static struct page_header_s *
fetch_thread_local_page(struct page_header_s *page)
{
    struct page_header_s *mypage = (struct page_header_s *)
        (((char *)page) + _STM_TL2.gs_value);

    if (!mypage->thread_local_copy) {
        /* make a thread-local copy of that page, by remapping the page
           back to its underlying page and manually copying the data. */
        uint64_t fileofs = ((char *)mypage) - stm_object_pages;

        if (remap_file_pages((void *)mypage, 4096, 0, fileofs / 4096,
                             MAP_PAGES_FLAGS) < 0) {
            perror("remap_file_pages in write_barrier");
            abort();
        }
        pagecopy(mypage, page);
        mypage->thread_local_copy = 1;
    }
    return mypage;
}

void _stm_write_barrier_slowpath(object_t *object)
{
    stm_read(object);

    struct page_header_s *page;
    page = (struct page_header_s *)(((uintptr_t)object) & ~4095);
    assert(2 <= page->obj_word_size);
    assert(page->obj_word_size < LARGE_OBJECT_WORDS);

    uint32_t byte_ofs16 = (((char *)object) - (char *)page) / 16;
    uint32_t pgoff = (((char *)page) - stm_object_pages) / 4096;

    page = fetch_thread_local_page(page);

    uint32_t write_log_index = page->write_log_index_cache;
    struct write_history_s *log = _STM_TL2.writes_by_this_transaction;

    if (write_log_index >= log->nb_updates ||
            log->updates[write_log_index].pgoff != pgoff) {
        /* make a new entry for this page in the write log */
        write_log_index = log->nb_updates++;
        assert(log->nb_updates <= _STM_TL2.nb_updates_max);  // XXX resize
        log->updates[write_log_index].pgoff = pgoff;
        log->updates[write_log_index].bitmask[0] = 0;
        log->updates[write_log_index].bitmask[1] = 0;
        log->updates[write_log_index].bitmask[2] = 0;
        log->updates[write_log_index].bitmask[3] = 0;
    }

    assert(byte_ofs16 < 256);
    log->updates[write_log_index].bitmask[byte_ofs16 / 64] |=
        (1UL << (byte_ofs16 & 63));
}

#if 0
char *_stm_alloc_next_page(size_t i)
{
    struct page_header_s *newpage = _stm_reserve_page();
    newpage->modif_head = 0xff;
    newpage->kind = i;      /* object size in words */
    newpage->version = 0;   /* a completely new page doesn't need a version */
    stm_local.alloc[i].next = ((char *)(newpage + 1)) + (i * 8);
    stm_local.alloc[i].end = ((char *)newpage) + 4096;
    assert(stm_local.alloc[i].next <= stm_local.alloc[i].end);
    return (char *)(newpage + 1);
}

struct object_s *stm_allocate(size_t size)
{
    assert(size % 8 == 0);
    size_t i = size / 8;
    assert(2 <= i && i < LARGE_OBJECT_WORDS);//XXX
    struct alloc_for_size_s *alloc = &stm_local.alloc[i];

    char *p = alloc->next;
    alloc->next += size;
    if (alloc->next > alloc->end)
        p = _stm_alloc_next_page(i);

    struct object_s *result = (struct object_s *)p;
    result->modified = stm_transaction_version;
    /*result->modif_next is uninitialized*/
    result->flags = 0x42;   /* for debugging */
    return result;
}


unsigned char stm_get_read_marker_number(void)
{
    return (unsigned char)(uintptr_t)stm_current_read_markers;
}

void stm_set_read_marker_number(uint8_t num)
{
    char *stm_pages = ((char *)stm_shared_descriptor) + 4096;
    uintptr_t delta = ((uintptr_t)stm_pages) >> 4;
    struct _read_marker_s *crm = (struct _read_marker_s *)stm_local.read_markers;
    stm_current_read_markers = crm - delta;
    assert(stm_get_read_marker_number() == 0);
    stm_current_read_markers += num;
}

static void clear_all_read_markers(void)
{
    /* set the largest possible read marker number, to find the last
       possible read_marker to clear */
    stm_set_read_marker_number(0xff);

    uint64_t page_index = stm_shared_descriptor->index_page_never_used;
    char *o = ((char *)stm_shared_descriptor) + page_index * 4096;
    char *m = (char *)get_current_read_marker((struct object_s *)o);
    size_t length = m - (char *)stm_local.read_markers;
    length = (length + 4095) & ~4095;

    int r = madvise(stm_local.read_markers, length, MADV_DONTNEED);
    if (r != 0) {
        perror("madvise() failure");
        abort();
    }
    stm_set_read_marker_number(1);
}
#endif

void stm_setup(void)
{
    if (sizeof(char *) != 8) {
        fprintf(stderr, "Only works on 64-bit Linux systems for now!\n");
        abort();
    }
    if (NB_PAGES > (1ull << 32)) {
        fprintf(stderr, "Cannot use more than 1<<32 pages of memory");
        abort();
    }

    /* For now, just prepare to make the layout given at the start of
       this file, with the RM pages interleaved with the L-0 blocks.
       The actual L-0-RM pages are allocated by each thread. */
    uint64_t addr_rm_base = (NB_PAGES + 1) * 4096UL;
    uint64_t addr_object_pages = addr_rm_base << 4;

    stm_object_pages = mmap((void *)addr_object_pages,
                            (NB_PAGES * 4096UL) * NB_THREADS,
                            PROT_READ | PROT_WRITE,
                            MAP_PAGES_FLAGS | MAP_FIXED, -1, 0);
    if (stm_object_pages == MAP_FAILED) {
        perror("mmap stm_object_pages failed");
        abort();
    }
    stm_shared_descriptor.index_page_never_used = 0;
}

void _stm_teardown(void)
{
    munmap((void *)stm_object_pages, (NB_PAGES * 4096UL) * NB_THREADS);
    stm_object_pages = NULL;
    memset(&stm_shared_descriptor, 0, sizeof(stm_shared_descriptor));
}

static void set_gs_register(uint64_t value)
{
    int result = syscall(SYS_arch_prctl, ARCH_SET_GS, &value);
    assert(result == 0);
}

static char *local_L0_pages(uint64_t gs_value)
{
    return (char *)(gs_value - 4096UL);
}

static char *local_RM_pages(uint64_t gs_value)
{
    return (char*)gs_value + (((uint64_t)stm_object_pages) >> 4);
}

int stm_setup_thread(void)
{
    int res;
    int thnum = stm_next_thread_index;
    int tries = 2 * NB_THREADS;
    uint64_t gs_value;
    while (1) {
        thnum %= NB_THREADS;
        stm_next_thread_index = thnum + 1;

        if (!--tries) {
            fprintf(stderr, "too many threads or too many non-fitting mmap\n");
            abort();
        }

        gs_value = (thnum+1) * 4096UL * NB_PAGES;

        if (mmap(local_L0_pages(gs_value), 2 * 4096UL, PROT_NONE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) == MAP_FAILED) {
            thnum++;
            continue;
        }

        uint64_t nb_rm_pages = (NB_PAGES + 15) >> 4;
        if (mmap(local_RM_pages(gs_value), nb_rm_pages * 4096UL,
                 PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) == MAP_FAILED) {
            munmap(local_L0_pages(gs_value), 2 * 4096UL);
            thnum++;
            continue;
        }
        break;
    }

    res = mprotect(local_L0_pages(gs_value), 4096, PROT_READ | PROT_WRITE);
    if (res < 0) {
        perror("remap_file_pages in stm_setup_thread");
        abort();
    }
    res = remap_file_pages(stm_object_pages + gs_value, NB_PAGES * 4096UL, 0,
                           0, MAP_PAGES_FLAGS);
    if (res < 0) {
        perror("remap_file_pages in stm_setup_thread");
        abort();
    }
    set_gs_register(gs_value);
    _STM_TL2.gs_value = gs_value;
    _STM_TL1.read_marker = 1;
    return thnum;
}

void _stm_restore_state_for_thread(int thread_num)
{
    uint64_t gs_value = (thread_num + 1) * 4096UL * NB_PAGES;
    set_gs_register(gs_value);
    assert(_STM_TL2.gs_value == gs_value);
}

void _stm_teardown_thread(void)
{
    uint64_t gs_value = _STM_TL2.gs_value;
    uint64_t nb_rm_pages = (NB_PAGES + 15) >> 4;
    munmap(local_RM_pages(gs_value), nb_rm_pages * 4096UL);
    munmap(local_L0_pages(gs_value), 2 * 4096UL);
    /* accessing _STM_TL2 is invalid here */
}

#if 0
static size_t get_obj_size_in_words(struct page_header_s *page)
{
    size_t result = page->kind;
    assert(2 <= result && result < LARGE_OBJECT_WORDS);
    return result;
}

static
struct object_s *get_object_in_page(struct page_header_s *page, size_t index)
{
    /* Slight complication here, because objects are aligned to 8 bytes,
       but we divides their page offset by 16 to fit a byte (4096/16 =
       256) and reduce memory overhead of the read markers.  Objects are
       at least 16 bytes in size, so there is no ambiguity.  Example for
       objects of 24 bytes of the organization inside a page (each word
       of the first line is 8 bytes):

       [HDR][OBJ.ECT.24][OBJ.ECT.24][OBJ.ECT.24][OBJ.ECT.24][..
       0        (16)     32      48     (64)     80      96

       The second line is all possible offsets, which are multiples of
       16.  They are the rounded-down version of the real offsets.
       object and round it down to a mutiple of 16.  For objects of size
       24, the numbers in parenthesis above are not reachable this way.
       The number 255 is never reachable.  To go from the number to the
       object address, we have to add either 0 or 8.
    */
    size_t obj_size_in_words = get_obj_size_in_words(page);
    size_t offset = (index << 4) +
        ((index << 1) % obj_size_in_words == 0 ? 8 : 0);
    return (struct object_s *)(((char *)page) + offset);
}

static _Bool must_merge_page(struct page_header_s *page)
{
    /* The remote page was modified.  Look at the local page (at
       'page').  If 'page->version' is equal to:

       - stm_transaction_version: the local page was also modified in
       this transaction.  Then we need to merge.

       - stm_transaction_version - 1: the local page was not, strictly
       speaking, modified, but *new* objects have been written to it.
       In order not to loose them, ask for a merge too.
    */
    return ((uint32_t)(stm_transaction_version - page->version)) <= 1;
}

static int history_fast_forward(struct write_history_s *new, int conflict)
{
    /* XXX do a non-recursive version, which also should avoid repeated
       remap_file_pages() on the same local-index-ed page */
    if (stm_local.base_page_mapping != new->previous_older_transaction) {
        conflict = history_fast_forward(new->previous_older_transaction,
                                        conflict);
    }
    assert(stm_local.base_page_mapping == new->previous_older_transaction);

    uint64_t i, nb_updates = new->nb_updates;
    for (i = 0; i < nb_updates; i++) {
     retry:;
        /* new->updates[] is an array of pairs (local_index, new_pgoff) */
        uint32_t local_index = new->updates[i * 2 + 0];
        uint32_t new_pgoff   = new->updates[i * 2 + 1];
        struct page_header_s *page = get_page_by_local_index(local_index);
        struct page_header_s *mypage = page;

        if (!conflict && must_merge_page(page)) {
            /* If we have also modified this page, then we must merge our
               changes with the ones done at 'new_pgoff'.  In this case
               we map 'new_pgoff' at the local index 1. */
            page = get_page_by_local_index(1);
        }

        remap_file_pages((void *)page, 4096, 0, new_pgoff, MAP_PAGES_FLAGS);
        assert(page->pgoff == new_pgoff);

        if (conflict)
            continue;

        /* look for read-from-me, write-from-others conflicts */
        if (mypage == page) {
            /* only look for conflicts: for every object modified by the
               other transaction, check that it was not read by us. */
            size_t modif_index = page->modif_head;
            while (modif_index != 0xff) {
                struct object_s *obj = get_object_in_page(page, modif_index);
                assert(obj->flags == 0x42);
                if (_stm_was_read(obj)) {
                    fprintf(stderr, "# conflict: %p\n", obj);
                    conflict = 1;
                    break;
                }
                modif_index = obj->modif_next;
            }
        }
        else {
            /* Merge two versions of the page: for every object modified
               by the other transaction, check that it was not read by us,
               and then copy it over into our own page at 'mypage'. */
            size_t obj_size = get_obj_size_in_words(page) << 3;
            uint64_t diff_to_mypage = ((char *)mypage) - (char *)page;
            size_t modif_index = page->modif_head;
            while (modif_index != 0xff) {
                struct object_s *obj = get_object_in_page(page, modif_index);
                struct object_s *myobj = (struct object_s *)
                    (((char *)obj) + diff_to_mypage);
                assert(obj->flags == 0x42);
                assert(myobj->flags == 0x42);
                if (_stm_was_read(myobj)) {
                    fprintf(stderr, "# conflict: %p\n", myobj);
                    conflict = 1;
                    goto retry;
                }
                memcpy(myobj, obj, obj_size);
                modif_index = obj->modif_next;
            }
        }
    }
    stm_local.base_page_mapping = new;
    return conflict;
}

void stm_start_transaction(void)
{
    struct shared_descriptor_s *d = stm_shared_descriptor;
    unsigned int v = __sync_fetch_and_add(&d->next_transaction_version, 2u);
    assert(v <= 0xffff);//XXX
    assert((v & 1) == 0);       /* EVEN number */
    assert(v >= 2);
    stm_transaction_version = v;

    struct write_history_s *cur = NULL;
    if (stm_local.writes_by_this_transaction != NULL) {
        cur = stm_local.writes_by_this_transaction;
        char *next, *page_limit = (char *)cur;
        page_limit += 4096 - (((uintptr_t)page_limit) & 4095);
        next = (char *)(cur + 1) + 8 * cur->nb_updates;
        if (page_limit - next < sizeof(struct write_history_s) + 8)
            cur = NULL;
        else
            cur = (struct write_history_s *)next;
    }
    if (cur == NULL) {
        cur = _reserve_page_write_history();
    }
    assert(cur != d->most_recent_committed_transaction);
    cur->previous_older_transaction = NULL;
    cur->transaction_version = stm_transaction_version;
    cur->nb_updates = 0;
    stm_local.writes_by_this_transaction = cur;

    struct write_history_s *hist = d->most_recent_committed_transaction;
    if (hist != stm_local.base_page_mapping) {
        history_fast_forward(hist, 1);
    }

    int i;
    for (i = 2; i < LARGE_OBJECT_WORDS; i++) {
        struct page_header_s *page;
        char *ptr = stm_local.alloc[i].next;
        if (ptr != NULL) {
            page = (struct page_header_s *)(((uintptr_t)ptr) & ~4095);
            page->version = stm_transaction_version - 1;
            /* ^^^ this is one of the only writes to shared memory;
               usually it is read-only */
        }
    }
}

_Bool stm_stop_transaction(void)
{
    struct shared_descriptor_s *d = stm_shared_descriptor;
    assert(stm_local.writes_by_this_transaction != NULL);
    int conflict = 0;
    //fprintf(stderr, "stm_stop_transaction\n");

    struct write_history_s *cur_head = stm_local.writes_by_this_transaction;
    struct write_history_s *cur_tail = cur_head;
    while (cur_tail->previous_older_transaction != NULL) {
        cur_tail = cur_tail->previous_older_transaction;
    }

    while (1) {
        struct write_history_s *hist = d->most_recent_committed_transaction;
        if (hist != stm_local.base_page_mapping) {
            conflict = history_fast_forward(hist, 0);
            if (conflict)
                break;
            else
                continue;   /* retry from the start of the loop */
        }
        assert(cur_head == stm_local.writes_by_this_transaction);
        cur_tail->previous_older_transaction = hist;
        if (__sync_bool_compare_and_swap(&d->most_recent_committed_transaction,
                                         hist, cur_head))
            break;
    }

    if (stm_get_read_marker_number() < 0xff) {
        stm_current_read_markers++;
    }
    else {
        clear_all_read_markers();
    }
    return !conflict;
}
#endif
