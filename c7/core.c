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
#include "nursery.h"
#include "pages.h"



#if defined(__i386__) || defined(__x86_64__)
#  define HAVE_FULL_EXCHANGE_INSN
#endif



char *object_pages;
static int num_threads_started;
uint8_t write_locks[READMARKER_END - READMARKER_START];


/************************************************************/


static void spin_loop(void)
{
    asm("pause" : : : "memory");
}


static void write_fence(void)
{
#if defined(__amd64__) || defined(__i386__)
    asm("" : : : "memory");
#else
#  error "Define write_fence() for your architecture"
#endif
}

/************************************************************/


/* a multi-reader, single-writer lock: transactions normally take a reader
   lock, so don't conflict with each other; when we need to do a global GC,
   we take a writer lock to "stop the world". */

rwticket rw_shared_lock;        /* the "GIL" */

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
    assert(!_STM_TL->need_abort);
    stm_stop_shared_lock();
}

void _stm_stop_safe_point(void)
{
    stm_start_shared_lock();
    if (_STM_TL->need_abort)
        stm_abort_transaction();
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
    ssize_t localpgoff = pgoff1 + NB_PAGES * _STM_TL->thread_num;
    ssize_t otherpgoff = pgoff1 + NB_PAGES * (1 - _STM_TL->thread_num);

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
    
    uintptr_t res = ptr - _STM_TL->thread_base;
    assert(FIRST_OBJECT_PAGE * 4096 <= res
           && res < NB_PAGES * 4096);
    return (object_t*)res;
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
            assert(write_locks[lock_idx]);
            write_locks[lock_idx] = 0;
            
            char *src = REAL_ADDRESS(local_base, item);
            char *dst = REAL_ADDRESS(remote_base, item);
            size_t size = stmcb_size((struct object_s*)src);
            memcpy(dst, src, size);
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
    
    LIST_APPEND(_STM_TL->old_objects_to_trace, obj);
    
    /* for old objects from the same transaction we don't need
       to privatize the page */
    if ((flag_page_private[pagenum] == UNCOMMITTED_SHARED_PAGE)
        || (obj->stm_flags & GCFLAG_NOT_COMMITTED)) {
        obj->stm_flags &= ~GCFLAG_WRITE_BARRIER;
        return;
    }

    /* privatize if SHARED_PAGE */
    /* xxx stmcb_size() is probably too slow */
    int pages = stmcb_size(real_address(obj)) / 4096;
    for (; pages >= 0; pages--)
        _stm_privatize(pagenum + pages);

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

    LIST_APPEND(_STM_TL->modified_objects, obj);
}




void stm_setup(void)
{
    memset(&rw_shared_lock, 0, sizeof(rwticket));

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
        flag_page_private[i] = PRIVATE_PAGE; /* nursery is private.
                                                or should it be UNCOMMITTED??? */
    
    num_threads_started = 0;
    index_page_never_used = FIRST_AFTER_NURSERY_PAGE;
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

    _STM_TL->nursery_current = (localchar_t*)(FIRST_NURSERY_PAGE * 4096);
    _STM_TL->shadow_stack = (object_t**)malloc(LENGTH_SHADOW_STACK * sizeof(void*));
    _STM_TL->shadow_stack_base = _STM_TL->shadow_stack;

    _STM_TL->old_objects_to_trace = stm_list_create();
    _STM_TL->uncommitted_pages = stm_list_create();
    
    _STM_TL->modified_objects = stm_list_create();
    _STM_TL->uncommitted_objects = stm_list_create();
    assert(!_STM_TL->running_transaction);
}

bool _stm_is_in_transaction(void)
{
    return _STM_TL->running_transaction;
}

void _stm_teardown_thread(void)
{
    assert(!rwticket_wrtrylock(&rw_shared_lock));
    assert(!rwticket_wrunlock(&rw_shared_lock));
    
    stm_list_free(_STM_TL->modified_objects);
    _STM_TL->modified_objects = NULL;

    assert(stm_list_is_empty(_STM_TL->uncommitted_objects));
    stm_list_free(_STM_TL->uncommitted_objects);
    _STM_TL->uncommitted_objects = NULL;           

    assert(_STM_TL->shadow_stack == _STM_TL->shadow_stack_base);
    free(_STM_TL->shadow_stack);

    assert(_STM_TL->old_objects_to_trace->count == 0);
    stm_list_free(_STM_TL->old_objects_to_trace);
    
    assert(_STM_TL->uncommitted_pages->count == 0);
    stm_list_free(_STM_TL->uncommitted_pages);

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

    assert(_STM_TL->thread_num == thread_num);
    assert(_STM_TL->thread_base == thread_base);
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


void stm_start_transaction(jmpbufptr_t *jmpbufptr)
{
    assert(!_STM_TL->running_transaction);

    stm_start_shared_lock();
    
    uint8_t old_rv = _STM_TL->transaction_read_version;
    _STM_TL->transaction_read_version = old_rv + 1;
    if (UNLIKELY(old_rv == 0xff))
        reset_transaction_read_version();

    assert(stm_list_is_empty(_STM_TL->modified_objects));
    
    nursery_on_start();
    
    _STM_TL->jmpbufptr = jmpbufptr;
    _STM_TL->running_transaction = 1;
    _STM_TL->need_abort = 0;
}


void stm_stop_transaction(void)
{
    assert(_STM_TL->running_transaction);
    stm_stop_shared_lock();
    stm_start_exclusive_lock();

    _STM_TL->jmpbufptr = NULL;          /* cannot abort any more */

    /* do a minor_collection,
       push uncommitted objects to other threads,
       make completely uncommitted pages SHARED,
    */
    nursery_on_commit();
    
    /* copy modified object versions to other threads */
    push_modified_to_other_threads();
    stm_list_clear(_STM_TL->modified_objects);

 
    _STM_TL->running_transaction = 0;
    stm_stop_exclusive_lock();
    fprintf(stderr, "%c", 'C'+_STM_TL->thread_num*32);
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
    assert(_STM_TL->running_transaction);
    

    /* reset shadowstack */
    _STM_TL->shadow_stack = _STM_TL->old_shadow_stack;

    nursery_on_abort();
    
    assert(_STM_TL->jmpbufptr != NULL);
    assert(_STM_TL->jmpbufptr != (jmpbufptr_t *)-1);   /* for tests only */
    _STM_TL->running_transaction = 0;
    stm_stop_shared_lock();
    fprintf(stderr, "%c", 'A'+_STM_TL->thread_num*32);

    /* reset all the modified objects (incl. re-adding GCFLAG_WRITE_BARRIER) */
    reset_modified_from_other_threads();
    stm_list_clear(_STM_TL->modified_objects);

    
    __builtin_longjmp(*_STM_TL->jmpbufptr, 1);
}
