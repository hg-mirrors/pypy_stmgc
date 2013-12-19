#define _GNU_SOURCE
#include <stdlib.h>
#include <stdio.h>
#include <sys/mman.h>
#include <errno.h>
#include <assert.h>
#include <string.h>

#include "core.h"
#include "pagecopy.h"


/* This file only works on 64-bit Linux for now.  The logic is based on
   remapping pages around, which can get a bit confusing.  Each "thread"
   runs in its own process, so that it has its own mapping.  The
   processes share an mmap of length NB_PAGES, which is created shared
   but anonymous, and passed to subprocesses by forking.

   The mmap's content does not depend on which process is looking at it:
   it contains what we'll call "mm pages", which is 4096 bytes of data
   at some file offset (which all processes agree on).  The term "pgoff"
   used below means such an offset.  It is a uint32_t expressed in units
   of 4096 bytes; so the underlying mmap is limited to 2**32 pages or
   16TB.

   The mm pages are then mapped in each process at some address, and
   their content is accessed with regular pointers.  We'll call such a
   page a "local page".  The term "local" is used because each process
   has its own, different mapping.  As it turns out, mm pages are
   initially mapped sequentially as local pages, but this changes over
   time.  To do writes in a transaction, the data containing the object
   is first duplicated --- so we allocate a fresh new mm page in the
   mmap file, and copy the contents to it.  Then we remap the new mm
   page over the *same* local page as the original.  So from this
   process' point of view, the object is still at the same address, but
   writes to it now happen to go to the new mm page instead of the old
   one.

   This is basically what happens automatically with fork() for regular
   memory; the difference is that at commit time, we try to publish the
   modified pages back for everybody to see.  This involves possibly
   merging changes done by other processes to other objects from the
   same page.

   The local pages are usually referenced by pointers, but may also be
   expressed as an index, called the "local index" of the page.
*/

#ifdef STM_TESTS
#  define NB_PAGES   (256*10)   // 10MB
#else
#  define NB_PAGES   (256*1024)   // 1GB
#endif
#define MAP_PAGES_FLAGS  (MAP_SHARED|MAP_ANONYMOUS)

#define CACHE_LINE_SIZE  128    // conservatively large value to avoid aliasing

#define PGKIND_NEVER_USED         0
#define LARGE_OBJECT_WORDS        36    /* range(2, LARGE_OBJECT_WORDS) */
#define PGKIND_FREED              0xff
#define PGKIND_WRITE_HISTORY      0xfe
#define PGKIND_SHARED_DESCRIPTOR  0xfd  /* only for the first mm page */

struct page_header_s {
    /* Every page starts with one such structure */
    uint16_t version;         /* when the data in the page was written */
    uint8_t modif_head;       /* head of a chained list of objects in this
                                 page that have modified == this->version */
    uint8_t kind;             /* either PGKIND_xxx or a number in
                                 range(2, LARGE_OBJECT_WORDS) */
    uint32_t pgoff;           /* the mm page offset */
};

struct read_marker_s {
    /* We associate a single byte to every object, by simply dividing
       the address of the object by 16.  The number in this single byte
       gives the last time we have read the object.  See stm_read(). */
    unsigned char c;
};

struct write_history_s {
    struct write_history_s *previous_older_transaction;
    uint16_t transaction_version;
    uint32_t nb_updates;
    uint32_t updates[];    /* pairs (local_index, new_pgoff) */
};

struct shared_descriptor_s {
    /* There is a single shared descriptor.  This regroups all data
       that needs to be dynamically shared among processes.  The
       first mm page is used for this. */
    union {
        struct page_header_s header;
        char _pad0[CACHE_LINE_SIZE];
    };
    union {
        uint64_t index_page_never_used;
        char _pad1[CACHE_LINE_SIZE];
    };
    union {
        unsigned int next_transaction_version;
        char _pad2[CACHE_LINE_SIZE];
    };
    union {
        struct write_history_s *most_recent_committed_transaction;
        char _pad3[CACHE_LINE_SIZE];
    };
};

struct alloc_for_size_s {
    char *next;
    char *end;
};

struct local_data_s {
    /* This is just a bunch of global variables, but during testing,
       we save it all away and restore different ones to simulate
       different forked processes. */
    char *read_markers;
    struct read_marker_s *current_read_markers;
    uint16_t transaction_version;
    struct write_history_s *base_page_mapping;
    struct write_history_s *writes_by_this_transaction;
    struct alloc_for_size_s alloc[LARGE_OBJECT_WORDS];
};

struct shared_descriptor_s *stm_shared_descriptor;
struct local_data_s stm_local;


void stm_read(struct object_s *object)
{
    stm_local.current_read_markers[((uintptr_t)object) >> 4].c =
        (unsigned char)(uintptr_t)stm_local.current_read_markers;
}

_Bool _stm_was_read(struct object_s *object)
{
    return (stm_local.current_read_markers[((uintptr_t)object) >> 4].c ==
            (unsigned char)(uintptr_t)stm_local.current_read_markers);
}

static struct read_marker_s *get_current_read_marker(struct object_s *object)
{
    return stm_local.current_read_markers + (((uintptr_t)object) >> 4);
}

void _stm_write_slowpath(struct object_s *);

void stm_write(struct object_s *object)
{
    if (__builtin_expect(object->modified != stm_local.transaction_version,
                         0))
        _stm_write_slowpath(object);
}

_Bool _stm_was_written(struct object_s *object)
{
    return (object->modified == stm_local.transaction_version);
}


struct page_header_s *_stm_reserve_page(void)
{
    /* Grab a free mm page, and map it into the address space.
       Return a pointer to it.  It has kind == PGKIND_FREED. */

    // XXX look in some free list first

    /* Return the index'th mm page, which is so far NEVER_USED.  It
       should never have been accessed so far, and be already mapped
       as the index'th local page. */
    struct shared_descriptor_s *d = stm_shared_descriptor;
    uint64_t index = __sync_fetch_and_add(&d->index_page_never_used, 1);
    if (index >= NB_PAGES) {
        fprintf(stderr, "Out of mmap'ed memory!\n");
        abort();
    }
    struct page_header_s *result = (struct page_header_s *)
        (((char *)stm_shared_descriptor) + index * 4096);
    assert(result->kind == PGKIND_NEVER_USED);
    result->kind = PGKIND_FREED;
    result->pgoff = index;
    return result;
}


static uint32_t get_pgoff(struct page_header_s *page)
{
    assert(page->pgoff > 0);
    assert(page->pgoff < NB_PAGES);
    return page->pgoff;
}

static uint32_t get_local_index(struct page_header_s *page)
{
    uint64_t index = ((char *)page) - (char *)stm_shared_descriptor;
    assert((index & 4095) == 0);
    index /= 4096;
    assert(0 < index && index < NB_PAGES);
    return index;
}

static struct page_header_s *get_page_by_local_index(uint32_t index)
{
    assert(0 < index && index < NB_PAGES);
    uint64_t ofs = ((uint64_t)index) * 4096;
    return (struct page_header_s *)(((char *)stm_shared_descriptor) + ofs);
}

void _stm_write_slowpath(struct object_s * object)
{
    stm_read(object);

    struct page_header_s *page;
    page = (struct page_header_s *)(((uintptr_t)object) & ~4095);
    assert(2 <= page->kind && page->kind < LARGE_OBJECT_WORDS);

    if (page->version != stm_local.transaction_version) {
        struct page_header_s *newpage = _stm_reserve_page();
        uint32_t old_pgoff = get_pgoff(page);
        uint32_t new_pgoff = get_pgoff(newpage);

        pagecopy(newpage, page);
        newpage->version = stm_local.transaction_version;
        newpage->modif_head = 0xff;
        newpage->pgoff = new_pgoff;
        assert(page->version != stm_local.transaction_version);
        assert(page->pgoff == old_pgoff);

        remap_file_pages((void *)page, 4096, 0, new_pgoff, MAP_PAGES_FLAGS);

        assert(page->version == stm_local.transaction_version);
        assert(page->pgoff == new_pgoff);

        struct write_history_s *cur = stm_local.writes_by_this_transaction;
        uint64_t i = cur->nb_updates++;
        size_t history_size_max = 4096 - (((uintptr_t)cur) & 4095);
        assert(sizeof(*cur) + cur->nb_updates * 8 <= history_size_max);
        cur->updates[i * 2 + 0] = get_local_index(page);
        cur->updates[i * 2 + 1] = new_pgoff;
    }
    object->modified = stm_local.transaction_version;
    object->modif_next = page->modif_head;
    page->modif_head = (uint8_t)(((uintptr_t)object) >> 4);
    assert(page->modif_head != 0xff);
}

char *_stm_alloc_next_page(size_t i)
{
    struct page_header_s *newpage = _stm_reserve_page();
    newpage->modif_head = 0xff;
    newpage->kind = i;   /* object size in words */
    newpage->version = stm_local.transaction_version;
    stm_local.alloc[i].next = ((char *)(newpage + 1)) + (i * 8);
    stm_local.alloc[i].end = ((char *)newpage) + 4096;
    assert(stm_local.alloc[i].next <= stm_local.alloc[i].end);
    return (char *)(newpage + 1);
}

struct object_s *stm_allocate(size_t size)
{
    assert(size % 8 == 0);
    size_t i = size / 8;
    assert(2 <= i && i < LARGE_OBJECT_WORDS);
    struct alloc_for_size_s *alloc = &stm_local.alloc[i];

    char *p = alloc->next;
    alloc->next += size;
    if (alloc->next > alloc->end)
        p = _stm_alloc_next_page(i);

    struct object_s *result = (struct object_s *)p;
    result->modified = stm_local.transaction_version;
    /*result->modif_next is uninitialized*/
    result->flags = 0x42;   /* for debugging */
    return result;
}


unsigned char stm_get_read_marker_number(void)
{
    return (unsigned char)(uintptr_t)stm_local.current_read_markers;
}

void stm_set_read_marker_number(uint8_t num)
{
    char *stm_pages = ((char *)stm_shared_descriptor) + 4096;
    uintptr_t delta = ((uintptr_t)stm_pages) >> 4;
    struct read_marker_s *crm = (struct read_marker_s *)stm_local.read_markers;
    stm_local.current_read_markers = crm - delta;
    assert(stm_get_read_marker_number() == 0);
    stm_local.current_read_markers += num;
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
    char *stm_pages = mmap(NULL, NB_PAGES*4096, PROT_READ|PROT_WRITE,
                           MAP_PAGES_FLAGS, -1, 0);
    if (stm_pages == MAP_FAILED) {
        perror("mmap stm_pages failed");
        abort();
    }
    assert(sizeof(struct shared_descriptor_s) <= 4096);
    stm_shared_descriptor = (struct shared_descriptor_s *)stm_pages;
    stm_shared_descriptor->header.kind = PGKIND_SHARED_DESCRIPTOR;
    /* the page at index 0 contains the '*stm_shared_descriptor' structure */
    /* the page at index 1 is reserved for history_fast_forward() */
    stm_shared_descriptor->index_page_never_used = 2;
    stm_shared_descriptor->next_transaction_version = 1;
}

void _stm_teardown(void)
{
    munmap((void *)stm_shared_descriptor, NB_PAGES*4096);
    stm_shared_descriptor = NULL;
}

void stm_setup_process(void)
{
    memset(&stm_local, 0, sizeof(stm_local));
    stm_local.read_markers = mmap(NULL, NB_PAGES*(4096 >> 4) + 1,
                                  PROT_READ|PROT_WRITE,
                                  MAP_PRIVATE|MAP_ANONYMOUS,
                                  -1, 0);
    if (stm_local.read_markers == MAP_FAILED) {
        perror("mmap stm_read_markers failed");
        abort();
    }

    stm_set_read_marker_number(42);
    assert(stm_get_read_marker_number() == 42);
    stm_set_read_marker_number(1);
}

void _stm_teardown_process(void)
{
    munmap((void *)stm_local.read_markers, NB_PAGES*(4096 >> 4) + 1);
    memset(&stm_local, 0, sizeof(stm_local));
}

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

        if (!conflict && page->version == stm_local.transaction_version) {
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
                assert(myobj->flags == 0x42); // || myobj->flags == 0);
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
    stm_local.transaction_version =
        __sync_fetch_and_add(&d->next_transaction_version, 1u);
    assert(stm_local.transaction_version <= 0xffff);

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
        struct page_header_s *newpage = _stm_reserve_page();
        newpage->kind = PGKIND_WRITE_HISTORY;
        cur = (struct write_history_s *)(newpage + 1);
    }
    cur->previous_older_transaction = NULL;
    cur->transaction_version = stm_local.transaction_version;
    cur->nb_updates = 0;
    stm_local.writes_by_this_transaction = cur;

    struct write_history_s *hist = d->most_recent_committed_transaction;
    if (hist != stm_local.base_page_mapping) {
        history_fast_forward(hist, 1);
    }
}

_Bool stm_stop_transaction(void)
{
    struct shared_descriptor_s *d = stm_shared_descriptor;
    assert(stm_local.writes_by_this_transaction != NULL);
    int conflict = 0;
    //fprintf(stderr, "stm_stop_transaction\n");

    while (1) {
        struct write_history_s *hist = d->most_recent_committed_transaction;
        if (hist != stm_local.base_page_mapping) {
            conflict = history_fast_forward(hist, 0);
            if (conflict)
                break;
            else
                continue;   /* retry from the start of the loop */
        }
        struct write_history_s *cur = stm_local.writes_by_this_transaction;
        cur->previous_older_transaction = hist;
        if (__sync_bool_compare_and_swap(&d->most_recent_committed_transaction,
                                         hist, cur))
            break;
    }

    if (stm_get_read_marker_number() < 0xff) {
        stm_local.current_read_markers++;
    }
    else {
        clear_all_read_markers();
    }
    return !conflict;
}

#ifdef STM_TESTS
struct local_data_s *_stm_save_local_state(void)
{
    uint64_t i, page_count = stm_shared_descriptor->index_page_never_used;
    uint32_t *pgoffs;
    struct local_data_s *p = malloc(sizeof(struct local_data_s) +
                                    page_count * sizeof(uint32_t));
    assert(p != NULL);
    memcpy(p, &stm_local, sizeof(stm_local));

    pgoffs = (uint32_t *)(p + 1);
    pgoffs[0] = page_count;
    for (i = 2; i < page_count; i++) {
        pgoffs[i] = get_pgoff(get_page_by_local_index(i));
    }

    return p;
}

void _stm_restore_local_state(struct local_data_s *p)
{
    uint64_t i, page_count;
    uint32_t *pgoffs;

    remap_file_pages((void *)stm_shared_descriptor, 4096 * NB_PAGES,
                     0, 0, MAP_PAGES_FLAGS);

    pgoffs = (uint32_t *)(p + 1);
    page_count = pgoffs[0];
    for (i = 2; i < page_count; i++) {
        struct page_header_s *page = get_page_by_local_index(i);
        remap_file_pages((void *)page, 4096, 0, pgoffs[i], MAP_PAGES_FLAGS);
        assert(get_pgoff(page) == pgoffs[i]);
    }

    memcpy(&stm_local, p, sizeof(struct local_data_s));
    free(p);
}
#endif
