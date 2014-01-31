#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

#include <pthread.h>

#include "core.h"
#include "list.h"
#include "reader_writer_lock.h"
#include "nursery.h"
#include "pages.h"
#include "stmsync.h"
#include "largemalloc.h"


char *object_pages;
static int num_threads_started;
uint8_t write_locks[READMARKER_END - READMARKER_START];
volatile uint8_t inevitable_lock __attribute__((aligned(64))); /* cache-line alignment */

struct _thread_local1_s* _stm_dbg_get_tl(int thread)
{
    if (thread == -1)
        return (struct _thread_local1_s*)real_address((object_t*)_STM_TL);
    return (struct _thread_local1_s*)REAL_ADDRESS(get_thread_base(thread), _STM_TL);
}

bool _stm_was_read_remote(char *base, object_t *obj)
{
    struct read_marker_s *marker = (struct read_marker_s *)
        (base + (((uintptr_t)obj) >> 4));
    struct _thread_local1_s *other_TL1 = (struct _thread_local1_s*)
        (base + (uintptr_t)_STM_TL);
    return (marker->rm == other_TL1->transaction_read_version);
}

bool _stm_was_read(object_t *obj)
{
    read_marker_t *marker = (read_marker_t *)(((uintptr_t)obj) >> 4);
    return (marker->rm == _STM_TL->transaction_read_version);
}

bool _stm_was_written(object_t *obj)
{
    /* if the obj was written to in the current transaction
       and doesn't trigger the write-barrier slowpath */
    return !(obj->stm_flags & GCFLAG_WRITE_BARRIER);
}



static void push_modified_to_other_threads()
{
    /* WE HAVE THE EXCLUSIVE LOCK HERE */
    
    struct stm_list_s *modified = _STM_TL->modified_objects;
    char *local_base = _STM_TL->thread_base;
    char *remote_base = get_thread_base(1 - _STM_TL->thread_num);
    bool conflicted = 0;
    
    STM_LIST_FOREACH(
        modified,
        ({
            if (!conflicted)
                conflicted = _stm_was_read_remote(remote_base, item);

            /* clear the write-lock */
            uintptr_t lock_idx = (((uintptr_t)item) >> 4) - READMARKER_START;
            assert(write_locks[lock_idx] == _STM_TL->thread_num + 1);
            write_locks[lock_idx] = 0;

            _stm_move_object(item,
                             REAL_ADDRESS(local_base, item),
                             REAL_ADDRESS(remote_base, item));
        }));
    
    if (conflicted) {
        struct _thread_local1_s *remote_TL = (struct _thread_local1_s *)
            REAL_ADDRESS(remote_base, _STM_TL);
        remote_TL->need_abort = 1;
    }
}



void _stm_write_slowpath(object_t *obj)
{
    uintptr_t pagenum = ((uintptr_t)obj) / 4096;
    assert(pagenum < NB_PAGES);
    assert(!_stm_is_young(obj));
    
    LIST_APPEND(_STM_TL->old_objects_to_trace, obj);
    
    /* for old objects from the same transaction we don't need
       to privatize the pages */
    if (obj->stm_flags & GCFLAG_NOT_COMMITTED) {
        obj->stm_flags &= ~GCFLAG_WRITE_BARRIER;
        return;
    }
    
    /* privatize if SHARED_PAGE */
    uintptr_t pagenum2, pages;
    if (obj->stm_flags & GCFLAG_SMALL) {
        pagenum2 = pagenum;
        pages = 1;
    } else {
        _stm_chunk_pages((struct object_s*)REAL_ADDRESS(get_thread_base(0), obj),
                         &pagenum2, &pages);
        assert(pagenum == pagenum2);
        /* assert(pages == (stmcb_size(real_address(obj)) + 4095) / 4096);
           not true if obj spans two pages, but is itself smaller than 1 */
    }
    
    for (pagenum2 += pages - 1; pagenum2 >= pagenum; pagenum2--)
        stm_pages_privatize(pagenum2);

    
    /* claim the write-lock for this object (XXX: maybe a fastpath
       for prev_owner == lock_num?) */
    uintptr_t lock_idx = (((uintptr_t)obj) >> 4) - READMARKER_START;
    uint8_t lock_num = _STM_TL->thread_num + 1;
    uint8_t prev_owner;
 retry:
    do {
        prev_owner = __sync_val_compare_and_swap(&write_locks[lock_idx],
                                               0, lock_num);
        
        /* if there was no lock-holder or we already have the lock */
        if ((!prev_owner) || (prev_owner == lock_num))
            break;

        if (_STM_TL->active == 2) {
            /* we must succeed! */
            _stm_dbg_get_tl(prev_owner - 1)->need_abort = 1;
            _stm_start_safe_point(0);
            /* XXX: not good, maybe should be signalled by other thread */
            usleep(1);
            _stm_stop_safe_point(0);
            goto retry;
        }
        /* XXXXXX */
        //_stm_start_semi_safe_point();
        //usleep(1);
        //_stm_stop_semi_safe_point();
        // try again.... XXX
        stm_abort_transaction();
        /* XXX: only abort if we are younger */
        spin_loop();
    } while (1);

    /* remove the write-barrier ONLY if we have the write-lock */
    obj->stm_flags &= ~GCFLAG_WRITE_BARRIER;
    
    if (prev_owner == 0) {
        /* otherwise, we have the lock and already added it to
           modified_objects / read-marker */
        stm_read(obj);
        LIST_APPEND(_STM_TL->modified_objects, obj);
    }
}

void _stm_setup_static_thread(void)
{
    int thread_num = __sync_fetch_and_add(&num_threads_started, 1);
    assert(thread_num < 2);  /* only 2 threads for now */

    _stm_restore_local_state(thread_num);

    _STM_TL->nursery_current = (localchar_t*)(FIRST_NURSERY_PAGE * 4096);
    memset((void*)real_address((object_t*)_STM_TL->nursery_current), 0x0,
           (FIRST_AFTER_NURSERY_PAGE - FIRST_NURSERY_PAGE) * 4096); /* clear nursery */
    
    _STM_TL->shadow_stack = NULL;
    _STM_TL->shadow_stack_base = NULL;

    _STM_TL->old_objects_to_trace = stm_list_create();
    
    _STM_TL->modified_objects = stm_list_create();
    _STM_TL->uncommitted_objects = stm_list_create();
    assert(!_STM_TL->active);
    _stm_assert_clean_tl();
}

void stm_setup(void)
{
    _stm_reset_shared_lock();
    _stm_reset_pages();

    inevitable_lock = 0;
    
    /* Check that some values are acceptable */
    assert(4096 <= ((uintptr_t)_STM_TL));
    assert(((uintptr_t)_STM_TL) == ((uintptr_t)_STM_TL));
    assert(((uintptr_t)_STM_TL) + sizeof(*_STM_TL) <= 8192);
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
        /* Make a "hole" at _STM_TL / _STM_TL */
        memset(REAL_ADDRESS(thread_base, _STM_TL), 0, sizeof(*_STM_TL));

        /* Pages in range(2, FIRST_READMARKER_PAGE) are never used */
        if (FIRST_READMARKER_PAGE > 2)
            mprotect(thread_base + 8192, (FIRST_READMARKER_PAGE - 2) * 4096UL,
                         PROT_NONE);

        struct _thread_local1_s *th =
            (struct _thread_local1_s *)REAL_ADDRESS(thread_base, _STM_TL);

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

    for (i = FIRST_NURSERY_PAGE; i < FIRST_AFTER_NURSERY_PAGE; i++)
        stm_set_page_flag(i, PRIVATE_PAGE); /* nursery is private.
                                                or should it be UNCOMMITTED??? */
    
    num_threads_started = 0;

    assert(HEAP_PAGES < NB_PAGES - FIRST_AFTER_NURSERY_PAGE);
    assert(HEAP_PAGES > 10);

    uintptr_t first_heap = stm_pages_reserve(HEAP_PAGES);
    char *heap = REAL_ADDRESS(get_thread_base(0), first_heap * 4096UL); 
    assert(memset(heap, 0xcd, HEAP_PAGES * 4096)); // testing
    stm_largemalloc_init(heap, HEAP_PAGES * 4096UL);

    for (i = 0; i < NB_THREADS; i++) {
        _stm_setup_static_thread();
    }
}



void _stm_teardown_static_thread(int thread_num)
{
    _stm_restore_local_state(thread_num);
    
    _stm_assert_clean_tl();
    _stm_reset_shared_lock();
    
    stm_list_free(_STM_TL->modified_objects);
    _STM_TL->modified_objects = NULL;

    assert(stm_list_is_empty(_STM_TL->uncommitted_objects));
    stm_list_free(_STM_TL->uncommitted_objects);

    assert(_STM_TL->shadow_stack == _STM_TL->shadow_stack_base);
    free(_STM_TL->shadow_stack);

    assert(_STM_TL->old_objects_to_trace->count == 0);
    stm_list_free(_STM_TL->old_objects_to_trace);

    _stm_restore_local_state(-1); // invalid
}

void stm_teardown(void)
{
    for (; num_threads_started > 0; num_threads_started--) {
        _stm_teardown_static_thread(num_threads_started - 1);
    }
    
    assert(inevitable_lock == 0);
    munmap(object_pages, TOTAL_MEMORY);
    _stm_reset_pages();
    memset(write_locks, 0, sizeof(write_locks));
    object_pages = NULL;
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
    _STM_TL->transaction_read_version = 1;
}


void stm_become_inevitable(char* msg)
{
    if (_STM_TL->active == 2)
        return;
    assert(_STM_TL->active == 1);
    fprintf(stderr, "%c", 'I'+_STM_TL->thread_num*32);

    uint8_t our_lock = _STM_TL->thread_num + 1;
    do {
        _stm_start_safe_point(LOCK_COLLECT);
        _stm_stop_safe_point(LOCK_COLLECT|LOCK_EXCLUSIVE);

        if (!inevitable_lock)
            break;

        _stm_start_safe_point(LOCK_EXCLUSIVE|LOCK_COLLECT);
        _stm_stop_safe_point(LOCK_COLLECT);
    } while (1);

    inevitable_lock = our_lock;
    _STM_TL->active = 2;
    
    _stm_start_safe_point(LOCK_EXCLUSIVE|LOCK_COLLECT);
    _stm_stop_safe_point(LOCK_COLLECT);
}

void stm_start_inevitable_transaction()
{
    stm_start_transaction(NULL);
    stm_become_inevitable("stm_start_inevitable_transaction");
}

void stm_start_transaction(jmpbufptr_t *jmpbufptr)
{
    /* GS invalid before this point! */
    _stm_stop_safe_point(LOCK_COLLECT|THREAD_YIELD);
    
    assert(!_STM_TL->active);
    
    uint8_t old_rv = _STM_TL->transaction_read_version;
    _STM_TL->transaction_read_version = old_rv + 1;
    if (UNLIKELY(old_rv == 0xff))
        reset_transaction_read_version();

    assert(stm_list_is_empty(_STM_TL->modified_objects));
    
    nursery_on_start();
    
    _STM_TL->jmpbufptr = jmpbufptr;
    _STM_TL->active = 1;
    _STM_TL->need_abort = 0;
    
    fprintf(stderr, "%c", 'S'+_STM_TL->thread_num*32);
}


void stm_stop_transaction(void)
{
    assert(_STM_TL->active);

    /* do the minor_collection here and not in nursery_on_commit,
       since here we can still run concurrently with other threads
       as we don't hold the exclusive lock yet. */
    _stm_minor_collect();

    /* Some operations require us to have the EXCLUSIVE lock */
    if (_STM_TL->active == 1) {
        while (1) {
            _stm_start_safe_point(LOCK_COLLECT);
            usleep(1);          /* XXX: better algorithm that allows
                                   for waiting on a mutex */
            _stm_stop_safe_point(LOCK_COLLECT|LOCK_EXCLUSIVE);
            
            if (!inevitable_lock)
                break;

            _stm_start_safe_point(LOCK_COLLECT|LOCK_EXCLUSIVE);
            _stm_stop_safe_point(LOCK_COLLECT);
        }
        /* we have the exclusive lock */
    } else {
        /* inevitable! no other transaction could have committed
           or aborted us */
        _stm_start_safe_point(LOCK_COLLECT);
        _stm_stop_safe_point(LOCK_EXCLUSIVE|LOCK_COLLECT);
        inevitable_lock = 0;
    }

    _STM_TL->jmpbufptr = NULL;          /* cannot abort any more */

    /* push uncommitted objects to other threads */
    nursery_on_commit();
    
    /* copy modified object versions to other threads */
    push_modified_to_other_threads();
    stm_list_clear(_STM_TL->modified_objects);

 
    _STM_TL->active = 0;

    fprintf(stderr, "%c", 'C'+_STM_TL->thread_num*32);
    
    _stm_start_safe_point(LOCK_EXCLUSIVE|LOCK_COLLECT|THREAD_YIELD);
    /* GS invalid after this point! */
}


static void reset_modified_from_other_threads()
{
    /* pull the right versions from other threads in order
       to reset our pages as part of an abort */
    
    struct stm_list_s *modified = _STM_TL->modified_objects;
    char *local_base = _STM_TL->thread_base;
    char *remote_base = get_thread_base(1 - _STM_TL->thread_num);
    
    STM_LIST_FOREACH(
        modified,
        ({
            /* note: same as push_modified_to... but src/dst swapped
               TODO: unify both... */
            
             /* check at least the first page (required by move_obj() */
            assert(stm_get_page_flag((uintptr_t)item / 4096) == PRIVATE_PAGE);
            
            _stm_move_object(item,
                             REAL_ADDRESS(remote_base, item),
                             REAL_ADDRESS(local_base, item));
            
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
    assert(_STM_TL->active == 1);
    
    nursery_on_abort();
    
    assert(_STM_TL->jmpbufptr != NULL);
    assert(_STM_TL->jmpbufptr != (jmpbufptr_t *)-1);   /* for tests only */
    _STM_TL->active = 0;
    _STM_TL->need_abort = 0;

    /* reset all the modified objects (incl. re-adding GCFLAG_WRITE_BARRIER) */
    reset_modified_from_other_threads();
    stm_list_clear(_STM_TL->modified_objects);

    jmpbufptr_t *buf = _STM_TL->jmpbufptr; /* _STM_TL not valid during safe-point */
    fprintf(stderr, "%c", 'A'+_STM_TL->thread_num*32);
    
    _stm_start_safe_point(LOCK_COLLECT|THREAD_YIELD);
    /* GS invalid after this point! */
    
    __builtin_longjmp(*buf, 1);
}
