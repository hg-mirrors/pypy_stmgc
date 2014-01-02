#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <asm/prctl.h>
#include <sys/prctl.h>

#include "core.h"
#include "list.h"
#include "pagecopy.h"


#define NB_PAGES            (256*256)    // 256MB
#define NB_THREADS          2
#define MAP_PAGES_FLAGS     (MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE)
#define LARGE_OBJECT_WORDS  36


typedef TLPREFIX char localchar_t;
typedef TLPREFIX struct alloc_for_size_s alloc_for_size_t;
typedef TLPREFIX struct _thread_local2_s _thread_local2_t;


struct alloc_for_size_s {
    localchar_t *next;
    uint16_t start, stop;
    bool flag_partial_page;
};

struct _thread_local2_s {
    struct _thread_local1_s _tl1;
    int thread_num;
    char *thread_base;
    struct stm_list_s *modified_objects;
    struct stm_list_s *new_object_ranges;
    struct alloc_for_size_s alloc[LARGE_OBJECT_WORDS];
};
#define _STM_TL2            ((_thread_local2_t *)_STM_TL1)

enum { SHARED_PAGE=0, REMAPPING_PAGE, PRIVATE_PAGE };  /* flag_page_private */


static char *object_pages;
static char *undo_log_pages;
static char *undo_log_current;
static int num_threads_started, leader_thread_num;
static uintptr_t index_page_never_used;
static int next_write_version;
static int undo_lock;
static struct stm_list_s *global_history;
static uint16_t gh_write_version_first;
static uint16_t gh_write_version_last;
static uint8_t flag_page_private[NB_PAGES];   /* xxx_PAGE constants above */


/************************************************************/

static void spin_loop(void)
{
    asm("pause" : : : "memory");
}

static void acquire_lock(int *lock)
{
    while (__sync_lock_test_and_set(lock, 1) != 0) {
        while (*lock != 0)
            spin_loop();
    }
}

#define ACQUIRE_LOCK_IF(lock, condition)                \
({                                                      \
    bool _acquired = false;                             \
    while (condition) {                                 \
        if (__sync_lock_test_and_set(lock, 1) == 0) {   \
            if (condition)                              \
                _acquired = true;                       \
            else                                        \
                __sync_lock_release(lock);              \
            break;                                      \
        }                                               \
        spin_loop();                                    \
    }                                                   \
    _acquired;                                          \
})

static void release_lock(int *lock)
{
    __sync_lock_release(lock);
}

static void write_fence(void)
{
#if defined(__amd64__) || defined(__i386__)
    asm("" : : : "memory");
#else
#  error "Define write_fence() for your architecture"
#endif
}

static bool _stm_was_read(object_t *obj)
{
    read_marker_t *marker = (read_marker_t *)(((uintptr_t)obj) >> 4);
    return (marker->rm == _STM_TL1->transaction_read_version);
}


static void _stm_privatize(uintptr_t pagenum)
{
    if (flag_page_private[pagenum] == PRIVATE_PAGE)
        return;

    if (!__sync_bool_compare_and_swap(&flag_page_private[pagenum],
                                      SHARED_PAGE, REMAPPING_PAGE)) {
        while (flag_page_private[pagenum] == REMAPPING_PAGE)
            spin_loop();
        assert(flag_page_private[pagenum] == PRIVATE_PAGE);
        return;
    }

    ssize_t pgoff1 = pagenum;
    ssize_t pgoff2 = pagenum + NB_PAGES;
    ssize_t localpgoff = pgoff1 + NB_PAGES * _STM_TL2->thread_num;
    ssize_t otherpgoff = pgoff1 + NB_PAGES * (1 - _STM_TL2->thread_num);

    void *localpg = object_pages + localpgoff * 4096UL;
    void *otherpg = object_pages + otherpgoff * 4096UL;

    int res = remap_file_pages(localpg, 4096, 0, pgoff2, 0);
    if (res < 0) {
        perror("remap_file_pages");
        abort();
    }
    pagecopy(localpg, otherpg);
    write_fence();
    assert(flag_page_private[pagenum] == REMAPPING_PAGE);
    flag_page_private[pagenum] = PRIVATE_PAGE;
}


#define REAL_ADDRESS(object_pages, src)   ((object_pages) + (uintptr_t)(src))

static char *real_address(uintptr_t src)
{
    return REAL_ADDRESS(_STM_TL2->thread_base, src);
}

static char *get_thread_base(long thread_num)
{
    return object_pages + thread_num * (NB_PAGES * 4096UL);
}

void stm_abort_transaction(void);

enum detect_conflicts_e { CANNOT_CONFLICT, CAN_CONFLICT };

/* XXX this can be done by acquiring the undo_lock for much less time,
   but it needs to be carefully synchronized with _stm_write_slowpath().
   For now it must be called with the undo_lock acquired. */
static void update_to_current_version(enum detect_conflicts_e check_conflict)
{
    /* Loop over objects in 'global_history': if they have been
       read by the current transaction, the current transaction must
       abort; then copy them out of the leader's object space ---
       which may have been modified by the leader's uncommitted
       transaction; this case will be fixed afterwards.
    */
    bool conflict_found_or_dont_check = (check_conflict == CANNOT_CONFLICT);
    char *local_base = _STM_TL2->thread_base;
    char *remote_base = get_thread_base(1 - _STM_TL2->thread_num);
    struct stm_list_s *gh, *gh_next;

    assert(leader_thread_num != _STM_TL2->thread_num);

    for (gh = global_history; gh != NULL; gh = gh_next) {

        STM_LIST_FOREACH(gh, ({

            if (!conflict_found_or_dont_check)
                conflict_found_or_dont_check = _stm_was_read(item);

            char *dst = REAL_ADDRESS(local_base, item);
            char *src = REAL_ADDRESS(remote_base, item);
            char *src_rebased = src - (uintptr_t)local_base;
            size_t size = stm_object_size_rounded_up((object_t *)src_rebased);

            memcpy(dst + sizeof(char *),
                   src + sizeof(char *),
                   size - sizeof(char *));
        }));

        gh_next = gh->nextlist;
        stm_list_free(gh);
    }
    global_history = NULL;
    gh_write_version_first = 0xffff;
    gh_write_version_last = 0;

    /* Finally, loop over objects modified by the leader,
       and copy them out of the undo log.
    */
    char *undo = undo_log_pages;
    char *undo_end = undo_log_current;

    while (undo < undo_end) {

        char *src = undo;
        char *dst = *(char **)src;
        char *src_rebased = src - (uintptr_t)local_base;

        *(char **)src = *(char **)dst; /* fix the first word of the object in
                                         the undo log, for stm_object_size() */
        size_t size = stm_object_size_rounded_up((object_t *)src_rebased);

        memcpy(dst + sizeof(char *),
               src + sizeof(char *),
               size - sizeof(char *));

        undo += size;
    }
    undo_log_current = undo_log_pages;   /* make empty again */

    if (conflict_found_or_dont_check && check_conflict == CAN_CONFLICT) {
        release_lock(&undo_lock);
        stm_abort_transaction();
    }
}

static void maybe_update(enum detect_conflicts_e check_conflict)
{
    if (leader_thread_num != _STM_TL2->thread_num && global_history != NULL) {
        acquire_lock(&undo_lock);
        update_to_current_version(check_conflict);
        release_lock(&undo_lock);
    }
}


void _stm_write_slowpath(object_t *obj)
{
    maybe_update(CAN_CONFLICT);

    _stm_privatize(((uintptr_t)obj) / 4096);

    stm_read(obj);

    _STM_TL2->modified_objects = stm_list_append(_STM_TL2->modified_objects, obj);

    uint16_t wv = obj->write_version;
    obj->write_version = _STM_TL1->transaction_write_version;

    /* We only need to store a copy of the current version of the object if:
       - we are the leader;
       - the object is present in the global_history.
       The second condition is approximated by the following range check.
       Storing a few more objects than strictly needed is not really a problem.
    */
    /* XXX this can be done without acquiring the undo_lock at all,
       but we need more care in update_to_current_version(). */

    /* XXX can we avoid writing an unbounded number of copies of the
       same object in case we run a lot of transactions while the other
       thread is busy?  Unlikely case but in theory annoying.  Should
       we anyway bound the undo log's size to much less than NB_PAGES,
       and if full here, sleep?  Should the bound also count the size
       taken by the global_history lists? */
    if (ACQUIRE_LOCK_IF(&undo_lock,
            wv <= gh_write_version_last && wv >= gh_write_version_first
                && leader_thread_num == _STM_TL2->thread_num)) {
        /* record in the undo log a copy of the content of the object */
        size_t size = stm_object_size_rounded_up(obj);
        char *source = real_address((uintptr_t)obj);
        char *undo = undo_log_current;
        *((object_t **)undo) = obj;
        memcpy(undo + sizeof(object_t *),
               source + sizeof(object_t *),
               size - sizeof(object_t *));
        /*write_fence();*/
        undo_log_current = undo + size;
        release_lock(&undo_lock);
    }
}


uintptr_t _stm_reserve_page(void)
{
    /* Grab a free page, initially shared between the threads. */

    // XXX look in some free list first

    /* Return the index'th object page, which is so far never used. */
    uintptr_t index = __sync_fetch_and_add(&index_page_never_used, 1);
    if (index >= NB_PAGES) {
        fprintf(stderr, "Out of mmap'ed memory!\n");
        abort();
    }
    return index;
}

#define TO_RANGE(range, start, stop)                                    \
    ((range) = (object_t *)((start) | (((uintptr_t)(stop)) << 16)))

#define FROM_RANGE(start, stop, range)          \
    ((start) = (uint16_t)(uintptr_t)(range),    \
     (stop) = ((uintptr_t)(range)) >> 16)

localchar_t *_stm_alloc_next_page(size_t i)
{
    /* 'alloc->next' points to where the next allocation should go.  The
       present function is called instead when this next allocation is
       equal to 'alloc->stop'.  As we know that 'start', 'next' and
       'stop' are always nearby pointers, we play tricks and only store
       the lower 16 bits of 'start' and 'stop', so that the three
       variables plus some flags fit in 16 bytes.

       'flag_partial_page' is *cleared* to mean that the 'alloc'
       describes a complete page, so that it needs not be listed inside
       'new_object_ranges'.  In all other cases it is *set*.
    */
    uintptr_t page;
    localchar_t *result;
    alloc_for_size_t *alloc = &_STM_TL2->alloc[i];
    size_t size = i * 8;

    if (alloc->flag_partial_page) {
        /* record this range in 'new_object_ranges' */
        localchar_t *ptr1 = alloc->next - size - 1;
        object_t *range;
        TO_RANGE(range, alloc->start, alloc->stop);
        page = ((uintptr_t)ptr1) / 4096;
        _STM_TL2->new_object_ranges = stm_list_append(
            _STM_TL2->new_object_ranges, (object_t *)page);
        _STM_TL2->new_object_ranges = stm_list_append(
            _STM_TL2->new_object_ranges, range);
    }

    /* reserve a fresh new page */
    page = _stm_reserve_page();

    result = (localchar_t *)(page * 4096UL);
    alloc->start = (uintptr_t)result;
    alloc->stop = alloc->start + (4096 / size) * size;
    alloc->next = result + size;
    alloc->flag_partial_page = false;
    return result;
}

object_t *stm_allocate(size_t size)
{
    assert(size % 8 == 0);
    size_t i = size / 8;
    assert(2 <= i && i < LARGE_OBJECT_WORDS);//XXX
    alloc_for_size_t *alloc = &_STM_TL2->alloc[i];

    localchar_t *p = alloc->next;
    alloc->next = p + size;
    if ((uint16_t)(uintptr_t)p == alloc->stop)
        p = _stm_alloc_next_page(i);

    object_t *result = (object_t *)p;
    result->write_version = _STM_TL1->transaction_write_version;
    return result;
}


#define TOTAL_MEMORY          (NB_PAGES * 4096UL * (NB_THREADS + 1))
#define READMARKER_END        ((NB_PAGES * 4096UL) >> 4)
#define FIRST_OBJECT_PAGE     ((READMARKER_END + 4095) / 4096UL)
#define READMARKER_START      ((FIRST_OBJECT_PAGE * 4096UL) >> 4)
#define FIRST_READMARKER_PAGE (READMARKER_START / 4096UL)

void stm_setup(void)
{
    /* Check that some values are acceptable */
    assert(4096 <= ((uintptr_t)_STM_TL1));
    assert(((uintptr_t)_STM_TL1) == ((uintptr_t)_STM_TL2));
    assert(((uintptr_t)_STM_TL2) + sizeof(*_STM_TL2) <= 8192);
    assert(2 <= FIRST_READMARKER_PAGE);
    assert(FIRST_READMARKER_PAGE * 4096UL <= READMARKER_START);
    assert(READMARKER_START < READMARKER_END);
    assert(READMARKER_END <= 4096UL * FIRST_OBJECT_PAGE);
    assert(FIRST_OBJECT_PAGE < NB_PAGES);

    object_pages = mmap(NULL, TOTAL_MEMORY,
                        PROT_READ | PROT_WRITE,
                        MAP_PAGES_FLAGS, -1, 0);
    if (object_pages == MAP_FAILED) {
        perror("object_pages mmap");
        abort();
    }

    long i;
    for (i = 0; i < NB_THREADS; i++) {
        char *thread_base = get_thread_base(i);

        /* In each thread's section, the first page is where TLPREFIX'ed
           NULL accesses land.  We mprotect it so that accesses fail. */
        mprotect(thread_base, 4096, PROT_NONE);

        /* Fill the TLS page (page 1) with 0xDD */
        memset(REAL_ADDRESS(thread_base, 4096), 0xDD, 4096);
        /* Make a "hole" at _STM_TL1 / _STM_TL2 */
        memset(REAL_ADDRESS(thread_base, _STM_TL2), 0, sizeof(*_STM_TL2));

        _STM_TL2->thread_num = i;
        _STM_TL2->thread_base = thread_base;

        if (i > 0) {
            int res;
            res = remap_file_pages(thread_base + FIRST_OBJECT_PAGE * 4096UL,
                                   (NB_PAGES - FIRST_OBJECT_PAGE) * 4096UL,
                                   0, FIRST_OBJECT_PAGE, 0);
            if (res != 0) {
                perror("remap_file_pages");
                abort();
            }
        }
    }

    undo_log_pages = get_thread_base(NB_THREADS);
    mprotect(undo_log_pages, 4096, PROT_NONE);
    mprotect(undo_log_pages + (NB_PAGES - 1) * 4096UL, 4096, PROT_NONE);
    undo_log_pages += 4096;
    undo_log_current = undo_log_pages;

    num_threads_started = 0;
    index_page_never_used = FIRST_OBJECT_PAGE;
    next_write_version = 1;
    leader_thread_num = 0;
    global_history = NULL;
    gh_write_version_first = 0xffff;
    gh_write_version_last = 0;
}

#define INVALID_GS_VALUE  0xDDDDDDDDDDDDDDDDUL

static void set_gs_register(uint64_t value)
{
    int result = syscall(SYS_arch_prctl, ARCH_SET_GS, value);
    assert(result == 0);
}

void stm_setup_thread(void)
{
    int thread_num = __sync_fetch_and_add(&num_threads_started, 1);
    assert(thread_num < 2);  /* only 2 threads for now */

    char *thread_base = get_thread_base(thread_num);
    set_gs_register((uintptr_t)thread_base);

    assert(_STM_TL2->thread_num == thread_num);
    assert(_STM_TL2->thread_base == thread_base);

    _STM_TL2->modified_objects = stm_list_create();
}

void _stm_teardown_thread(void)
{
    stm_list_free(_STM_TL2->modified_objects);
    _STM_TL2->modified_objects = NULL;

    set_gs_register(INVALID_GS_VALUE);
}

void _stm_teardown(void)
{
    munmap(object_pages, TOTAL_MEMORY);
    object_pages = NULL;
    undo_log_pages = NULL;
    undo_log_current = NULL;
}


static void reset_transaction_read_version(void)
{
    /* force-reset all read markers to 0 */

    /* XXX measure the time taken by this madvise() and the following
       zeroing of pages done lazily by the kernel; compare it with using
       16-bit read_versions.
    */
    /* XXX try to use madvise() on smaller ranges of memory.  In my
       measures, we could gain a factor 2 --- not really more, even if
       the range of virtual addresses below is very large, as long as it
       is already mostly non-reserved pages.  (The following call keeps
       them non-reserved; apparently the kernel just skips them very
       quickly.)
    */
    int res = madvise(real_address(FIRST_READMARKER_PAGE * 4096UL),
                      (FIRST_OBJECT_PAGE - FIRST_READMARKER_PAGE) * 4096UL,
                      MADV_DONTNEED);
    if (res < 0) {
        perror("madvise");
        abort();
    }
    _STM_TL1->transaction_read_version = 0;
}

void stm_major_collection(void)
{
    abort();
}

void stm_start_transaction(jmp_buf *jmpbufptr)
{
    if (_STM_TL1->transaction_read_version == 0xff)
        reset_transaction_read_version();
    _STM_TL1->transaction_read_version++;
    _STM_TL1->jmpbufptr = NULL;

    while (1) {
        int wv = __sync_fetch_and_add(&next_write_version, 1);
        if (LIKELY(wv <= 0xffff)) {
            _STM_TL1->transaction_write_version = wv;
            break;
        }
        /* We run out of 16-bit numbers before we do the next major
           collection, which resets it.  XXX This case seems unlikely
           for now, but check if it could become a bottleneck at some
           point. */
        stm_major_collection();
    }
    assert(stm_list_is_empty(_STM_TL2->modified_objects));
    assert(stm_list_is_empty(_STM_TL2->new_object_ranges));

    maybe_update(CANNOT_CONFLICT);    /* no read object: cannot conflict */

    _STM_TL1->jmpbufptr = jmpbufptr;
}

static void update_new_objects_in_other_threads(uintptr_t pagenum,
                                                uint16_t start, uint16_t stop)
{
    size_t size = (uint16_t)(stop - start);
    assert(size <= 4096 - (start & 4095));
    assert((start & ~4095) == (uint16_t)(pagenum * 4096));

    int thread_num = _STM_TL2->thread_num;
    uintptr_t local_src = (pagenum * 4096UL) + (start & 4095);
    char *dst = REAL_ADDRESS(get_thread_base(1 - thread_num), local_src);
    char *src = REAL_ADDRESS(_STM_TL2->thread_base,           local_src);

    memcpy(dst, src, size);
}

void stm_stop_transaction(void)
{
    write_fence();   /* see later in this function for why */

    acquire_lock(&undo_lock);

    if (leader_thread_num != _STM_TL2->thread_num) {
        /* non-leader thread */
        if (global_history != NULL) {
            update_to_current_version(CAN_CONFLICT);
            assert(global_history == NULL);
        }

        /* steal leadership now */
        leader_thread_num = _STM_TL2->thread_num;
    }

    /* now we are the leader thread.  the leader can always commit */
    _STM_TL1->jmpbufptr = NULL;          /* cannot abort any more */
    undo_log_current = undo_log_pages;   /* throw away the content */

    /* add these objects to the global_history */
    _STM_TL2->modified_objects->nextlist = global_history;
    global_history = _STM_TL2->modified_objects;
    _STM_TL2->modified_objects = stm_list_create();

    uint16_t wv = _STM_TL1->transaction_write_version;
    if (wv < gh_write_version_last)  gh_write_version_last  = wv;
    if (wv > gh_write_version_first) gh_write_version_first = wv;

    /* walk the new_object_ranges and manually copy the new objects
       to the other thread's pages in the (hopefully rare) case that
       the page they belong to is already unshared */
    long i;
    struct stm_list_s *lst = _STM_TL2->new_object_ranges;
    for (i = stm_list_count(lst); i > 0; ) {
        i -= 2;
        uintptr_t pagenum = (uintptr_t)stm_list_item(lst, i);

        /* NB. the read next line should work even against a parallel
           thread, thanks to the lock acquisition we do earlier (see the
           beginning of this function).  Indeed, if this read returns
           SHARED_PAGE, then we know that the real value in memory was
           actually SHARED_PAGE at least at the time of the
           acquire_lock().  It may have been modified afterwards by a
           compare_and_swap() in the other thread, but then we know for
           sure that the other thread is seeing the last, up-to-date
           version of our data --- this is the reason of the
           write_fence() just before the acquire_lock().
        */
        if (flag_page_private[pagenum] != SHARED_PAGE) {
            object_t *range = stm_list_item(lst, i + 1);
            uint16_t start, stop;
            FROM_RANGE(start, stop, range);
            update_new_objects_in_other_threads(pagenum, start, stop);
        }
    }

    /* do the same for the partially-allocated pages */
    long j;
    for (j = 2; j < LARGE_OBJECT_WORDS; j++) {
        alloc_for_size_t *alloc = &_STM_TL2->alloc[j];
        uint16_t start = alloc->start;
        uint16_t cur = (uintptr_t)alloc->next;

        if (start == cur) {
            /* nothing to do: this page (or fraction thereof) was left
               empty by the previous transaction, and starts empty as
               well in the new transaction.  'flag_partial_page' is
               unchanged. */
        }
        else {
            uintptr_t pagenum = ((uintptr_t)(alloc->next - 1)) / 4096UL;
            /* for the new transaction, it will start here: */
            alloc->start = cur;

            if (alloc->flag_partial_page) {
                if (flag_page_private[pagenum] != SHARED_PAGE) {
                    update_new_objects_in_other_threads(pagenum, start, cur);
                }
            }
            else {
                /* we can skip checking flag_page_private[] in non-debug
                   builds, because the whole page can only contain
                   objects made by the just-finished transaction. */
                assert(flag_page_private[pagenum] == SHARED_PAGE);

                /* the next transaction will start with this page
                   containing objects that are now committed, so
                   we need to set this flag now */
                alloc->flag_partial_page = true;
            }
        }
    }

    release_lock(&undo_lock);
}

void stm_abort_transaction(void)
{
    long j;
    for (j = 2; j < LARGE_OBJECT_WORDS; j++) {
        alloc_for_size_t *alloc = &_STM_TL2->alloc[j];
        uint16_t num_allocated = ((uintptr_t)alloc->next) - alloc->start;
        alloc->next -= num_allocated;
    }
    stm_list_clear(_STM_TL2->new_object_ranges);
    stm_list_clear(_STM_TL2->modified_objects);
    assert(_STM_TL1->jmpbufptr != NULL);
    assert(_STM_TL1->jmpbufptr != (jmp_buf *)-1);   /* for tests only */
    longjmp(*_STM_TL1->jmpbufptr, 1);
}
