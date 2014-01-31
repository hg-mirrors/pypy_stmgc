#include <assert.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <sys/prctl.h>
#include <asm/prctl.h>
#include <semaphore.h>

#include "stmsync.h"
#include "core.h"
#include "reader_writer_lock.h"
#include "list.h"

#define INVALID_GS_VALUE  0x6D6D6D6D

/* a multi-reader, single-writer lock: transactions normally take a reader
   lock, so don't conflict with each other; when we need to do a global GC,
   we take a writer lock to "stop the world". */

rwticket rw_shared_lock;        /* the "GIL" */
rwticket rw_collection_lock;    /* for major collections */

sem_t static_thread_semaphore;
uint8_t static_threads[NB_THREADS]; /* 1 if running a pthread */
__thread struct _thread_local1_s *pthread_tl = NULL;




void _stm_acquire_tl_segment();
void _stm_release_tl_segment();

static void set_gs_register(uint64_t value)
{
    int result = syscall(SYS_arch_prctl, ARCH_SET_GS, value);
    assert(result == 0);
}

bool _stm_is_in_transaction(void)
{
    return pthread_tl->active;
}


void _stm_restore_local_state(int thread_num)
{
    if (thread_num == -1) {     /* mostly for debugging */
        set_gs_register(INVALID_GS_VALUE);
        return;
    }
    
    char *thread_base = get_thread_base(thread_num);
    set_gs_register((uintptr_t)thread_base);

    assert(_STM_TL->thread_num == thread_num);
    assert(_STM_TL->thread_base == thread_base);
}


void _stm_yield_thread_segment()
{
    _stm_release_tl_segment();
    
    /* release our static thread: */
    static_threads[_STM_TL->thread_num] = 0;
    sem_post(&static_thread_semaphore);
    
    _stm_restore_local_state(-1); /* invalid */
}

void _stm_grab_thread_segment()
{
    /* acquire a static thread: */
    sem_wait(&static_thread_semaphore);
    int thread_num = 0;
    while (1) {
        if (!__sync_lock_test_and_set(&static_threads[thread_num], 1))
            break;
        thread_num = (thread_num + 1) % NB_THREADS;
    }
    
    _stm_restore_local_state(thread_num);
    _stm_acquire_tl_segment();
}


void _stm_assert_clean_tl()
{
    /* between a pthread switch, these are the things
       that must be guaranteed */
    
    /* already set are
       thread_num, thread_base: to the current static thread
       nursery_current: nursery should be cleared
       active, need_abort: no transaction running
       modified_objects: empty
       alloc: re-usable by this thread
       uncommitted_objects: empty
       old_objects_to_trace: empty
       !!shadow_stack...: still belongs to previous thread
    */
    assert(stm_list_is_empty(_STM_TL->modified_objects));
    assert(stm_list_is_empty(_STM_TL->uncommitted_objects));
    assert(stm_list_is_empty(_STM_TL->old_objects_to_trace));

    assert(!_STM_TL->active);
    /* assert(!_STM_TL->need_abort); may happen, but will be cleared by
       start_transaction() */ 
    assert(_STM_TL->nursery_current == (localchar_t*)(FIRST_NURSERY_PAGE * 4096));
}

void _stm_acquire_tl_segment()
{
    /* makes tl-segment ours! */
    _stm_assert_clean_tl();

    _STM_TL->shadow_stack = pthread_tl->shadow_stack;
    _STM_TL->shadow_stack_base = pthread_tl->shadow_stack_base;
    _STM_TL->old_shadow_stack = pthread_tl->old_shadow_stack;
}

void _stm_release_tl_segment()
{
    /* makes tl-segment ours! */
    _stm_assert_clean_tl();

    pthread_tl->shadow_stack = _STM_TL->shadow_stack;
    pthread_tl->shadow_stack_base = _STM_TL->shadow_stack_base;
    pthread_tl->old_shadow_stack = _STM_TL->old_shadow_stack;
}

void stm_setup_pthread(void)
{
    struct _thread_local1_s* tl = malloc(sizeof(struct _thread_local1_s));
    assert(!pthread_tl);
    pthread_tl = tl;
    
    /* get us a clean thread segment */
    _stm_grab_thread_segment();
    _stm_assert_clean_tl();
    
    /* allocate shadow stack for this thread */
    _STM_TL->shadow_stack = (object_t**)malloc(LENGTH_SHADOW_STACK * sizeof(void*));
    _STM_TL->shadow_stack_base = _STM_TL->shadow_stack;

    /* copy everything from _STM_TL */
    memcpy(tl, REAL_ADDRESS(get_thread_base(_STM_TL->thread_num), _STM_TL),
           sizeof(struct _thread_local1_s));

    /* go into safe-point again: */
    _stm_yield_thread_segment();
}


void stm_teardown_pthread(void)
{
    free(pthread_tl->shadow_stack_base);
    
    free(pthread_tl);
    pthread_tl = NULL;
}





void _stm_reset_shared_lock()
{
    assert(!rwticket_wrtrylock(&rw_shared_lock));
    assert(!rwticket_wrunlock(&rw_shared_lock));

    memset(&rw_shared_lock, 0, sizeof(rwticket));

    assert(!rwticket_wrtrylock(&rw_collection_lock));
    assert(!rwticket_wrunlock(&rw_collection_lock));

    memset(&rw_collection_lock, 0, sizeof(rwticket));

    int i;
    for (i = 0; i < NB_THREADS; i++)
        assert(static_threads[i] == 0);
    memset(static_threads, 0, sizeof(static_threads));
    sem_init(&static_thread_semaphore, 0, NB_THREADS);
}

/* void stm_acquire_collection_lock() */
/* { */
/*     /\* we must have the exclusive lock here and */
/*        not the colletion lock!! *\/ */
/*     /\* XXX: for more than 2 threads, need a way */
/*        to signal other threads with need_major_collect */
/*        so that they don't leave COLLECT-safe-points */
/*        when this flag is set. Otherwise we simply */
/*        wait arbitrarily long until all threads reach */
/*        COLLECT-safe-points by chance at the same time. *\/ */
/*     while (1) { */
/*         if (!rwticket_wrtrylock(&rw_collection_lock)) */
/*             break;              /\* acquired! *\/ */
        
/*         stm_stop_exclusive_lock(); */
/*         usleep(1); */
/*         stm_start_exclusive_lock(); */
/*         if (_STM_TL->need_abort) { */
/*             stm_stop_exclusive_lock(); */
/*             stm_start_shared_lock(); */
/*             stm_abort_transaction(); */
/*         } */
/*     } */
/* } */

void stm_start_shared_lock(void)
{
    rwticket_rdlock(&rw_shared_lock); 
}

void stm_stop_shared_lock()
{
    rwticket_rdunlock(&rw_shared_lock); 
}

void stm_start_exclusive_lock(void)
{
    rwticket_wrlock(&rw_shared_lock);
}

void stm_stop_exclusive_lock(void)
{
    rwticket_wrunlock(&rw_shared_lock);
}

/* _stm_start_safe_point(LOCK_EXCLUSIVE|LOCK_COLLECT)
   -> release the exclusive lock and also the collect-read-lock

   THREAD_YIELD: gives up its (current thread's) GS segment
   so that other threads can grab it and run. This will
   make _STM_TL and all thread-local addresses unusable
   for the current thread. (requires LOCK_COLLECT)
*/
void _stm_start_safe_point(uint8_t flags)
{
    assert(IMPLY(flags & THREAD_YIELD, flags & LOCK_COLLECT));
    
    if (flags & LOCK_EXCLUSIVE)
        stm_stop_exclusive_lock();
    else
        stm_stop_shared_lock();
    
    if (flags & LOCK_COLLECT) {
        rwticket_rdunlock(&rw_collection_lock);
        
        if (flags & THREAD_YIELD) {
            _stm_yield_thread_segment();
        }
    }
}

/*
  _stm_stop_safe_point(LOCK_COLLECT|LOCK_EXCLUSIVE);
  -> reacquire the collect-read-lock and the exclusive lock

  THREAD_YIELD: wait until we get a GS segment assigned
  and then continue (requires LOCK_COLLECT)
 */
void _stm_stop_safe_point(uint8_t flags)
{
    assert(IMPLY(flags & THREAD_YIELD, flags & LOCK_COLLECT));
    if (flags & THREAD_YIELD) {
        _stm_grab_thread_segment();
    }
    
    if (flags & LOCK_EXCLUSIVE)
        stm_start_exclusive_lock();
    else
        stm_start_shared_lock();
    
    if (flags & LOCK_COLLECT) { /* if we released the collection lock */
        /* acquire read-collection. always succeeds because
           if there was a write-collection holder we would
           also not have gotten the shared_lock */
        rwticket_rdlock(&rw_collection_lock);
    }
    
    if (_STM_TL->active && _STM_TL->need_abort) {
        if (flags & LOCK_EXCLUSIVE) {
            /* restore to shared-mode with the collection lock */
            stm_stop_exclusive_lock();
            stm_start_shared_lock();
            stm_abort_transaction();
        } else {
            stm_abort_transaction();
        }
    }
}



