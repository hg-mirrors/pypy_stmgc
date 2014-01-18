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
#include <pthread.h>

#include "core.h"
#include "list.h"
#include "pagecopy.h"
#include "reader_writer_lock.h"


#define NB_PAGES            (256*256)    // 256MB
#define NB_THREADS          2
#define MAP_PAGES_FLAGS     (MAP_SHARED | MAP_ANONYMOUS | MAP_NORESERVE)
#define LARGE_OBJECT_WORDS  232  // XXX was 36
#define NB_NURSERY_PAGES    1024
#define LENGTH_SHADOW_STACK   163840


#define TOTAL_MEMORY          (NB_PAGES * 4096UL * NB_THREADS)
#define READMARKER_END        ((NB_PAGES * 4096UL) >> 4)
#define FIRST_OBJECT_PAGE     ((READMARKER_END + 4095) / 4096UL)
#define FIRST_NURSERY_PAGE    FIRST_OBJECT_PAGE
#define READMARKER_START      ((FIRST_OBJECT_PAGE * 4096UL) >> 4)
#define FIRST_READMARKER_PAGE (READMARKER_START / 4096UL)
#define FIRST_AFTER_NURSERY_PAGE  (FIRST_OBJECT_PAGE + NB_NURSERY_PAGES)



#if defined(__i386__) || defined(__x86_64__)
#  define HAVE_FULL_EXCHANGE_INSN
#endif

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
    bool running_transaction;
    bool need_abort;
    char *thread_base;
    struct stm_list_s *modified_objects;
    struct stm_list_s *new_object_ranges;
    struct alloc_for_size_s alloc[LARGE_OBJECT_WORDS];
    localchar_t *nursery_current;

    struct stm_list_s *old_objects_to_trace;
    /* pages newly allocated in the current transaction only containing
       uncommitted objects */
    struct stm_list_s *uncommitted_pages;
};
#define _STM_TL2            ((_thread_local2_t *)_STM_TL1)

enum {
    /* unprivatized page seen by all threads */
    SHARED_PAGE=0,

    /* page being in the process of privatization */
    REMAPPING_PAGE,

    /* page private for each thread */
    PRIVATE_PAGE,

    /* set for SHARED pages that only contain objects belonging
       to the current transaction, so the whole page is not
       visible yet for other threads */
    UNCOMMITTED_SHARED_PAGE,
};  /* flag_page_private */


static char *object_pages;
static int num_threads_started;
static uintptr_t index_page_never_used;
static struct stm_list_s *volatile pending_updates;
static uint8_t flag_page_private[NB_PAGES];   /* xxx_PAGE constants above */
static uint8_t write_locks[READMARKER_END - READMARKER_START];

/************************************************************/
uintptr_t _stm_reserve_page(void);
void stm_abort_transaction(void);
localchar_t *_stm_alloc_next_page(size_t i);
void mark_page_as_uncommitted(uintptr_t pagenum);

static void spin_loop(void)
{
    asm("pause" : : : "memory");
}

#if 0
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
#endif

static void write_fence(void)
{
#if defined(__amd64__) || defined(__i386__)
    asm("" : : : "memory");
#else
#  error "Define write_fence() for your architecture"
#endif
}

/************************************************************/

rwticket rw_shared_lock;

/* a multi-reader, single-writer lock: transactions normally take a reader
   lock, so don't conflict with each other; when we need to do a global GC,
   we take a writer lock to "stop the world".  Note the initializer here,
   which should give the correct priority for stm_possible_safe_point(). */


struct tx_descriptor *in_single_thread = NULL;

void stm_start_shared_lock(void)
{
    rwticket_rdlock(&rw_shared_lock);
}

void stm_stop_shared_lock(void)
{
    rwticket_rdunlock(&rw_shared_lock);
}

void stm_stop_exclusive_lock(void)
{
    rwticket_wrunlock(&rw_shared_lock);
}

void stm_start_exclusive_lock(void)
{
    rwticket_wrlock(&rw_shared_lock);
}

void _stm_start_safe_point(void)
{
    assert(!_STM_TL2->need_abort);
    stm_stop_shared_lock();
}

void _stm_stop_safe_point(void)
{
    stm_start_shared_lock();
    if (_STM_TL2->need_abort)
        stm_abort_transaction();
}

bool _stm_was_read_remote(char *base, object_t *obj)
{
    struct read_marker_s *marker = (struct read_marker_s *)
        (base + (((uintptr_t)obj) >> 4));
    struct _thread_local1_s *other_TL1 = (struct _thread_local1_s*)
        (base + (uintptr_t)_STM_TL1);
    return (marker->rm == other_TL1->transaction_read_version);
}


bool _stm_was_read(object_t *obj)
{
    read_marker_t *marker = (read_marker_t *)(((uintptr_t)obj) >> 4);
    return (marker->rm == _STM_TL1->transaction_read_version);
}

bool _stm_was_written(object_t *obj)
{
    /* if the obj was written to in the current transaction
       and doesn't trigger the write-barrier slowpath */
    return !(obj->stm_flags & GCFLAG_WRITE_BARRIER);
}


object_t *_stm_allocate_old(size_t size)
{
    assert(size <= 4096);
    localchar_t* addr = (localchar_t*)(_stm_reserve_page() * 4096);
    object_t* o = (object_t*)addr;
    o->stm_flags |= GCFLAG_WRITE_BARRIER;
    return o;
}

object_t *stm_allocate_prebuilt(size_t size)
{
    return _stm_allocate_old(size);  /* XXX */
}


static void _stm_privatize(uintptr_t pagenum)
{
    if (flag_page_private[pagenum] == PRIVATE_PAGE)
        return;

#ifdef HAVE_FULL_EXCHANGE_INSN
    /* use __sync_lock_test_and_set() as a cheaper alternative to
       __sync_bool_compare_and_swap(). */
    int previous = __sync_lock_test_and_set(&flag_page_private[pagenum],
                                            REMAPPING_PAGE);
    if (previous == PRIVATE_PAGE) {
        flag_page_private[pagenum] = PRIVATE_PAGE;
        return;
    }
    bool was_shared = (previous == SHARED_PAGE);
#else
    bool was_shared = __sync_bool_compare_and_swap(&flag_page_private[pagenum],
                                                  SHARED_PAGE, REMAPPING_PAGE);
#endif
    if (!was_shared) {
        while (1) {
            uint8_t state = ((uint8_t volatile *)flag_page_private)[pagenum];
            if (state != REMAPPING_PAGE) {
                assert(state == PRIVATE_PAGE);
                break;
            }
            spin_loop();
        }
        return;
    }

    ssize_t pgoff1 = pagenum;
    ssize_t pgoff2 = pagenum + NB_PAGES;
    ssize_t localpgoff = pgoff1 + NB_PAGES * _STM_TL2->thread_num;
    ssize_t otherpgoff = pgoff1 + NB_PAGES * (1 - _STM_TL2->thread_num);

    void *localpg = object_pages + localpgoff * 4096UL;
    void *otherpg = object_pages + otherpgoff * 4096UL;

    // XXX should not use pgoff2, but instead the next unused page in
    // thread 2, so that after major GCs the next dirty pages are the
    // same as the old ones
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

static struct object_s *real_address(object_t *src)
{
    return (struct object_s*)REAL_ADDRESS(_STM_TL2->thread_base, src);
}


static char *get_thread_base(long thread_num)
{
    return object_pages + thread_num * (NB_PAGES * 4096UL);
}

bool _stm_is_young(object_t *o)
{
    assert((uintptr_t)o >= FIRST_NURSERY_PAGE * 4096);
    return (uintptr_t)o < FIRST_AFTER_NURSERY_PAGE * 4096;
}


char *_stm_real_address(object_t *o)
{
    if (o == NULL)
        return NULL;
    assert(FIRST_OBJECT_PAGE * 4096 <= (uintptr_t)o
           && (uintptr_t)o < NB_PAGES * 4096);
    return (char*)real_address(o);
}

object_t *_stm_tl_address(char *ptr)
{
    if (ptr == NULL)
        return NULL;
    
    uintptr_t res = ptr - _STM_TL2->thread_base;
    assert(FIRST_OBJECT_PAGE * 4096 <= res
           && res < NB_PAGES * 4096);
    return (object_t*)res;
}



static void push_modified_to_other_threads()
{
    /* WE HAVE THE EXCLUSIVE LOCK HERE */
    
    struct stm_list_s *modified = _STM_TL2->modified_objects;
    char *local_base = _STM_TL2->thread_base;
    char *remote_base = get_thread_base(1 - _STM_TL2->thread_num);
    bool conflicted = 0;
    
    STM_LIST_FOREACH(
        modified,
        ({
            if (!conflicted)
                conflicted = _stm_was_read_remote(remote_base, item);

            /* clear the write-lock */
            uintptr_t lock_idx = (((uintptr_t)item) >> 4) - READMARKER_START;
            assert(write_locks[lock_idx]);
            write_locks[lock_idx] = 0;
            
            char *src = REAL_ADDRESS(local_base, item);
            char *dst = REAL_ADDRESS(remote_base, item);
            size_t size = stmcb_size((struct object_s*)src);
            memcpy(dst, src, size);
        }));
    
    if (conflicted) {
        struct _thread_local2_s *remote_TL2 = (struct _thread_local2_s *)
            REAL_ADDRESS(remote_base, _STM_TL2);
        remote_TL2->need_abort = 1;
    }
}





void _stm_write_slowpath(object_t *obj)
{
    uintptr_t pagenum = ((uintptr_t)obj) / 4096;
    assert(pagenum < NB_PAGES);

    _STM_TL2->old_objects_to_trace = stm_list_append
        (_STM_TL2->old_objects_to_trace, obj);
    
    /* for old objects from the same transaction we don't need
       to privatize the page */
    if ((flag_page_private[pagenum] == UNCOMMITTED_SHARED_PAGE)
        || (obj->stm_flags & GCFLAG_NOT_COMMITTED)) {
        obj->stm_flags &= ~GCFLAG_WRITE_BARRIER;
        return;
    }

    /* privatize if SHARED_PAGE */
    _stm_privatize(pagenum);

    /* claim the write-lock for this object */
    uintptr_t lock_idx = (((uintptr_t)obj) >> 4) - READMARKER_START;
    uint8_t previous;
    while ((previous = __sync_lock_test_and_set(&write_locks[lock_idx], 1))) {
        /* XXXXXX */
        //_stm_start_semi_safe_point();
        usleep(1);
        //_stm_stop_semi_safe_point();
        //if (!(previous = __sync_lock_test_and_set(&write_locks[lock_idx], 1))) 
        //    break;
        stm_abort_transaction();
        /* XXX: only abort if we are younger */
        spin_loop();
    }

    obj->stm_flags &= ~GCFLAG_WRITE_BARRIER;
    stm_read(obj);

    _STM_TL2->modified_objects = stm_list_append
        (_STM_TL2->modified_objects, obj);
}


uintptr_t _stm_reserve_page(void)
{
    /* Grab a free page, initially shared between the threads. */

    // XXX look in some free list first

    /* Return the index'th object page, which is so far never used. */
    uintptr_t index = __sync_fetch_and_add(&index_page_never_used, 1);
    assert(flag_page_private[index] == SHARED_PAGE);
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

localchar_t *_stm_alloc_old(size_t size)
{
    size_t size_class = size / 8;
    alloc_for_size_t *alloc = &_STM_TL2->alloc[size_class];
    localchar_t *result;
    
    if ((uint16_t)((uintptr_t)alloc->next) == alloc->stop)
        result = _stm_alloc_next_page(size_class);
    else {
        result = alloc->next;
        alloc->next += size;
    }

    return result;
}

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

    /* if (alloc->flag_partial_page) { */
    /*     /\* record this range in 'new_object_ranges' *\/ */
    /*     localchar_t *ptr1 = alloc->next - size - 1; */
    /*     object_t *range; */
    /*     TO_RANGE(range, alloc->start, alloc->stop); */
    /*     page = ((uintptr_t)ptr1) / 4096; */
    /*     _STM_TL2->new_object_ranges = stm_list_append( */
    /*         _STM_TL2->new_object_ranges, (object_t *)page); */
    /*     _STM_TL2->new_object_ranges = stm_list_append( */
    /*         _STM_TL2->new_object_ranges, range); */
    /* } */

    /* reserve a fresh new page */
    page = _stm_reserve_page();

    /* mark as UNCOMMITTED_... */
    mark_page_as_uncommitted(page);

    result = (localchar_t *)(page * 4096UL);
    alloc->start = (uintptr_t)result;
    alloc->stop = alloc->start + (4096 / size) * size;
    alloc->next = result + size;
    alloc->flag_partial_page = false;
    return result;
}


void mark_page_as_uncommitted(uintptr_t pagenum)
{
    flag_page_private[pagenum] = UNCOMMITTED_SHARED_PAGE;
    _STM_TL2->uncommitted_pages = stm_list_append
        (_STM_TL2->uncommitted_pages, (object_t*)pagenum);
}

void trace_if_young(object_t **pobj)
{
    if (*pobj == NULL)
        return;
    if (!_stm_is_young(*pobj))
        return;

    /* the location the object moved to is at an 8b offset */
    localchar_t *temp = ((localchar_t *)(*pobj)) + 8;
    object_t * TLPREFIX *pforwarded = (object_t* TLPREFIX *)temp;
    if ((*pobj)->stm_flags & GCFLAG_MOVED) {
        *pobj = *pforwarded;
        return;
    }

    /* move obj to somewhere else */
    size_t size = stmcb_size(real_address(*pobj));
    object_t *moved = (object_t*)_stm_alloc_old(size);
    
    memcpy((void*)real_address(moved),
           (void*)real_address(*pobj),
           size);

    (*pobj)->stm_flags |= GCFLAG_MOVED;
    *pforwarded = moved;
    *pobj = moved;
    
    _STM_TL2->old_objects_to_trace = stm_list_append
        (_STM_TL2->old_objects_to_trace, moved);
}

void minor_collect()
{
    /* visit shadowstack & add to old_obj_to_trace */
    object_t **current = _STM_TL1->shadow_stack;
    object_t **base = _STM_TL1->shadow_stack_base;
    while (current-- != base) {
        trace_if_young(current);
    }
    
    /* visit old_obj_to_trace until empty */
    struct stm_list_s *old_objs = _STM_TL2->old_objects_to_trace;
    while (!stm_list_is_empty(old_objs)) {
        object_t *item = stm_list_pop_item(old_objs);

        assert(!_stm_is_young(item));
        assert(!(item->stm_flags & GCFLAG_WRITE_BARRIER));
        
        /* re-add write-barrier */
        item->stm_flags |= GCFLAG_WRITE_BARRIER;
        
        stmcb_trace(real_address(item), trace_if_young);
        old_objs = _STM_TL2->old_objects_to_trace;
    }

    
    // also move objects to PRIVATE_PAGE pages, but then
    // also add the GCFLAG_NOT_COMMITTED to these objects.
    
    /* clear nursery */
    localchar_t *nursery_base = (localchar_t*)(FIRST_NURSERY_PAGE * 4096);
    memset((void*)real_address((object_t*)nursery_base), 0x0,
           _STM_TL2->nursery_current - nursery_base);
    _STM_TL2->nursery_current = nursery_base;
}

localchar_t *collect_and_reserve(size_t size)
{
    _stm_start_safe_point();
    minor_collect();
    _stm_stop_safe_point();

    localchar_t *current = _STM_TL2->nursery_current;
    _STM_TL2->nursery_current = current + size;
    return current;
}

object_t *stm_allocate(size_t size)
{
    assert(_STM_TL2->running_transaction);
    assert(size % 8 == 0);
    size_t i = size / 8;
    assert(2 <= i && i < LARGE_OBJECT_WORDS);//XXX
    assert(2 <= i && i < NB_NURSERY_PAGES * 4096);//XXX

    localchar_t *current = _STM_TL2->nursery_current;
    localchar_t *new_current = current + size;
    _STM_TL2->nursery_current = new_current;
    assert((uintptr_t)new_current < (1L << 32));
    if ((uintptr_t)new_current > FIRST_AFTER_NURSERY_PAGE * 4096) {
        current = collect_and_reserve(size);
    }

    object_t *result = (object_t *)current;
    return result;
}




void stm_setup(void)
{
    memset(&rw_shared_lock, 0, sizeof(rwticket));

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

        /* Pages in range(2, FIRST_READMARKER_PAGE) are never used */
        if (FIRST_READMARKER_PAGE > 2)
            mprotect(thread_base + 8192, (FIRST_READMARKER_PAGE - 2) * 4096UL,
                         PROT_NONE);

        struct _thread_local2_s *th =
            (struct _thread_local2_s *)REAL_ADDRESS(thread_base, _STM_TL2);

        th->thread_num = i;
        th->thread_base = thread_base;

        if (i > 0) {
            int res;
            res = remap_file_pages(
                    thread_base + FIRST_AFTER_NURSERY_PAGE * 4096UL,
                    (NB_PAGES - FIRST_AFTER_NURSERY_PAGE) * 4096UL,
                    0, FIRST_AFTER_NURSERY_PAGE, 0);

            if (res != 0) {
                perror("remap_file_pages");
                abort();
            }
        }
    }

    num_threads_started = 0;
    index_page_never_used = FIRST_AFTER_NURSERY_PAGE;
    pending_updates = NULL;
}

#define INVALID_GS_VALUE  0x6D6D6D6D

static void set_gs_register(uint64_t value)
{
    int result = syscall(SYS_arch_prctl, ARCH_SET_GS, value);
    assert(result == 0);
}

void stm_setup_thread(void)
{
    int thread_num = __sync_fetch_and_add(&num_threads_started, 1);
    assert(thread_num < 2);  /* only 2 threads for now */

    _stm_restore_local_state(thread_num);

    _STM_TL2->nursery_current = (localchar_t*)(FIRST_NURSERY_PAGE * 4096);
    _STM_TL1->shadow_stack = (object_t**)malloc(LENGTH_SHADOW_STACK * sizeof(void*));
    _STM_TL1->shadow_stack_base = _STM_TL1->shadow_stack;

    _STM_TL2->old_objects_to_trace = stm_list_create();
    _STM_TL2->uncommitted_pages = stm_list_create();
    
    _STM_TL2->modified_objects = stm_list_create();
    assert(!_STM_TL2->running_transaction);
}

bool _stm_is_in_transaction(void)
{
    return _STM_TL2->running_transaction;
}

void _stm_teardown_thread(void)
{
    assert(!rwticket_wrtrylock(&rw_shared_lock));
    assert(!rwticket_wrunlock(&rw_shared_lock));
    
    stm_list_free(_STM_TL2->modified_objects);
    _STM_TL2->modified_objects = NULL;

    assert(_STM_TL1->shadow_stack == _STM_TL1->shadow_stack_base);
    free(_STM_TL1->shadow_stack);

    assert(_STM_TL2->old_objects_to_trace->count == 0);
    stm_list_free(_STM_TL2->old_objects_to_trace);
    
    assert(_STM_TL2->uncommitted_pages->count == 0);
    stm_list_free(_STM_TL2->uncommitted_pages);

    set_gs_register(INVALID_GS_VALUE);
}

void _stm_teardown(void)
{
    munmap(object_pages, TOTAL_MEMORY);
    memset(flag_page_private, 0, sizeof(flag_page_private));
    memset(write_locks, 0, sizeof(write_locks));
    object_pages = NULL;
}

void _stm_restore_local_state(int thread_num)
{
    char *thread_base = get_thread_base(thread_num);
    set_gs_register((uintptr_t)thread_base);

    assert(_STM_TL2->thread_num == thread_num);
    assert(_STM_TL2->thread_base == thread_base);
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
    int res = madvise((void*)real_address
                      ((object_t*) (FIRST_READMARKER_PAGE * 4096UL)),
                      (FIRST_OBJECT_PAGE - FIRST_READMARKER_PAGE) * 4096UL,
                      MADV_DONTNEED);
    if (res < 0) {
        perror("madvise");
        abort();
    }
    _STM_TL1->transaction_read_version = 1;
}

void stm_major_collection(void)
{
    assert(_STM_TL2->running_transaction);
    abort();
}

void stm_start_transaction(jmpbufptr_t *jmpbufptr)
{
    assert(!_STM_TL2->running_transaction);

    stm_start_shared_lock();
    
    uint8_t old_rv = _STM_TL1->transaction_read_version;
    _STM_TL1->transaction_read_version = old_rv + 1;
    if (UNLIKELY(old_rv == 0xff))
        reset_transaction_read_version();

    assert(stm_list_is_empty(_STM_TL2->modified_objects));
    assert(stm_list_is_empty(_STM_TL2->old_objects_to_trace));
    stm_list_clear(_STM_TL2->uncommitted_pages);

    _STM_TL1->jmpbufptr = jmpbufptr;
    _STM_TL2->running_transaction = 1;
    _STM_TL2->need_abort = 0;
}

#if 0
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
    abort();
}
#endif

void stm_stop_transaction(void)
{
    assert(_STM_TL2->running_transaction);
    stm_stop_shared_lock();
    stm_start_exclusive_lock();

    _STM_TL1->jmpbufptr = NULL;          /* cannot abort any more */

    minor_collect();
    
    /* copy modified object versions to other threads */
    push_modified_to_other_threads();
    stm_list_clear(_STM_TL2->modified_objects);

    /* uncommitted_pages */
    long j;
    for (j = 2; j < LARGE_OBJECT_WORDS; j++) {
        alloc_for_size_t *alloc = &_STM_TL2->alloc[j];
        uint16_t start = alloc->start;
        uint16_t cur = (uintptr_t)alloc->next;
        if (start == cur)
            continue;
        uintptr_t pagenum = ((uintptr_t)(alloc->next - 1)) / 4096UL;
        if (flag_page_private[pagenum] == UNCOMMITTED_SHARED_PAGE) {
            /* mark it as empty so it doesn't get used in the next
               transaction */
            /* XXX: flag_partial_page!! */
            alloc->start = 0;
            alloc->next = 0;
            alloc->stop = 0;
        }
    }
    
    STM_LIST_FOREACH(
        _STM_TL2->uncommitted_pages,
        ({
            uintptr_t pagenum = (uintptr_t)item;
            flag_page_private[pagenum] = SHARED_PAGE;
        }));
    stm_list_clear(_STM_TL2->uncommitted_pages);

    
    /* /\* walk the new_object_ranges and manually copy the new objects */
    /*    to the other thread's pages in the (hopefully rare) case that */
    /*    the page they belong to is already unshared *\/ */
    /* long i; */
    /* struct stm_list_s *lst = _STM_TL2->new_object_ranges; */
    /* for (i = stm_list_count(lst); i > 0; ) { */
    /*     i -= 2; */
    /*     uintptr_t pagenum = (uintptr_t)stm_list_item(lst, i); */

    /*     /\* NB. the read next line should work even against a parallel */
    /*        thread, thanks to the lock acquisition we do earlier (see the */
    /*        beginning of this function).  Indeed, if this read returns */
    /*        SHARED_PAGE, then we know that the real value in memory was */
    /*        actually SHARED_PAGE at least at the time of the */
    /*        acquire_lock().  It may have been modified afterwards by a */
    /*        compare_and_swap() in the other thread, but then we know for */
    /*        sure that the other thread is seeing the last, up-to-date */
    /*        version of our data --- this is the reason of the */
    /*        write_fence() just before the acquire_lock(). */
    /*     *\/ */
    /*     if (flag_page_private[pagenum] != SHARED_PAGE) { */
    /*         object_t *range = stm_list_item(lst, i + 1); */
    /*         uint16_t start, stop; */
    /*         FROM_RANGE(start, stop, range); */
    /*         update_new_objects_in_other_threads(pagenum, start, stop); */
    /*     } */
    /* } */
    
    /* /\* do the same for the partially-allocated pages *\/ */
    /* long j; */
    /* for (j = 2; j < LARGE_OBJECT_WORDS; j++) { */
    /*     alloc_for_size_t *alloc = &_STM_TL2->alloc[j]; */
    /*     uint16_t start = alloc->start; */
    /*     uint16_t cur = (uintptr_t)alloc->next; */

    /*     if (start == cur) { */
    /*         /\* nothing to do: this page (or fraction thereof) was left */
    /*            empty by the previous transaction, and starts empty as */
    /*            well in the new transaction.  'flag_partial_page' is */
    /*            unchanged. *\/ */
    /*     } */
    /*     else { */
    /*         uintptr_t pagenum = ((uintptr_t)(alloc->next - 1)) / 4096UL; */
    /*         /\* for the new transaction, it will start here: *\/ */
    /*         alloc->start = cur; */

    /*         if (alloc->flag_partial_page) { */
    /*             if (flag_page_private[pagenum] != SHARED_PAGE) { */
    /*                 update_new_objects_in_other_threads(pagenum, start, cur); */
    /*             } */
    /*         } */
    /*         else { */
    /*             /\* we can skip checking flag_page_private[] in non-debug */
    /*                builds, because the whole page can only contain */
    /*                objects made by the just-finished transaction. *\/ */
    /*             assert(flag_page_private[pagenum] == SHARED_PAGE); */

    /*             /\* the next transaction will start with this page */
    /*                containing objects that are now committed, so */
    /*                we need to set this flag now *\/ */
    /*             alloc->flag_partial_page = true; */
    /*         } */
    /*     } */
    /* } */

    _STM_TL2->running_transaction = 0;
    stm_stop_exclusive_lock();
    fprintf(stderr, "%c", 'C'+_STM_TL2->thread_num*32);
}


static void reset_modified_from_other_threads()
{
    /* pull the right versions from other threads in order
       to reset our pages as part of an abort */
    
    struct stm_list_s *modified = _STM_TL2->modified_objects;
    char *local_base = _STM_TL2->thread_base;
    char *remote_base = get_thread_base(1 - _STM_TL2->thread_num);
    
    STM_LIST_FOREACH(
        modified,
        ({
            /* note: same as push_modified_to... but src/dst swapped
               XXX: unify both... */
            
            char *dst = REAL_ADDRESS(local_base, item);
            char *src = REAL_ADDRESS(remote_base, item);
            size_t size = stmcb_size((struct object_s*)src);
            memcpy(dst, src, size);

            /* copying from the other thread re-added the
               WRITE_BARRIER flag */
            assert(item->stm_flags & GCFLAG_WRITE_BARRIER);

            /* write all changes to the object before we release the
               write lock below */
            write_fence();
            
            /* clear the write-lock */
            uintptr_t lock_idx = (((uintptr_t)item) >> 4) - READMARKER_START;
            assert(write_locks[lock_idx]);
            write_locks[lock_idx] = 0;
        }));
}


void stm_abort_transaction(void)
{
    /* here we hold the shared lock as a reader or writer */
    assert(_STM_TL2->running_transaction);
    

    /* clear old_objects_to_trace (they will have the WRITE_BARRIER flag
       set because the ones we care about are also in modified_objects) */
    stm_list_clear(_STM_TL2->old_objects_to_trace);

    /* clear the nursery */
    localchar_t *nursery_base = (localchar_t*)(FIRST_NURSERY_PAGE * 4096);
    memset((void*)real_address((object_t*)nursery_base), 0x0,
           _STM_TL2->nursery_current - nursery_base);
    _STM_TL2->nursery_current = nursery_base;

    /* unreserve uncommitted_pages and mark them as SHARED again */
        /* STM_LIST_FOREACH(_STM_TL2->uncommitted_pages, ({ */
        /*         uintptr_t pagenum = (uintptr_t)item; */
        /*         flag_page_private[pagenum] = SHARED_PAGE; */
        /*     })); */
    stm_list_clear(_STM_TL2->uncommitted_pages);


    /* XXX: forget about GCFLAG_UNCOMMITTED objects  */
    
    /* long j; */
    /* for (j = 2; j < LARGE_OBJECT_WORDS; j++) { */
    /*     alloc_for_size_t *alloc = &_STM_TL2->alloc[j]; */
    /*     uint16_t num_allocated = ((uintptr_t)alloc->next) - alloc->start; */
    /*     alloc->next -= num_allocated; */
    /* } */
    /* stm_list_clear(_STM_TL2->new_object_ranges); */
    
    
    assert(_STM_TL1->jmpbufptr != NULL);
    assert(_STM_TL1->jmpbufptr != (jmpbufptr_t *)-1);   /* for tests only */
    _STM_TL2->running_transaction = 0;
    stm_stop_shared_lock();
    fprintf(stderr, "%c", 'A'+_STM_TL2->thread_num*32);

    /* reset all the modified objects (incl. re-adding GCFLAG_WRITE_BARRIER) */
    reset_modified_from_other_threads();
    stm_list_clear(_STM_TL2->modified_objects);

    
    __builtin_longjmp(*_STM_TL1->jmpbufptr, 1);
}
