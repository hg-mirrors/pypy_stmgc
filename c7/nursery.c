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
#include "nursery.h"
#include "pages.h"
#include "stmsync.h"
#include "largemalloc.h"

void stm_major_collection(void)
{
    assert(_STM_TL->active);
    abort();
}


bool _stm_is_young(object_t *o)
{
    assert((uintptr_t)o >= FIRST_NURSERY_PAGE * 4096);
    return (uintptr_t)o < FIRST_AFTER_NURSERY_PAGE * 4096;
}


object_t *_stm_allocate_old(size_t size)
{
    object_t* o = stm_large_malloc(size);
    memset(real_address(o), 0, size);
    o->stm_flags |= GCFLAG_WRITE_BARRIER;
    return o;
}

object_t *stm_allocate_prebuilt(size_t size)
{
    return _stm_allocate_old(size);  /* XXX */
}

localchar_t *_stm_alloc_next_page(size_t size_class)
{
    /* may return uninitialized pages */
    
    /* 'alloc->next' points to where the next allocation should go.  The
       present function is called instead when this next allocation is
       equal to 'alloc->stop'.  As we know that 'start', 'next' and
       'stop' are always nearby pointers, we play tricks and only store
       the lower 16 bits of 'start' and 'stop', so that the three
       variables plus some flags fit in 16 bytes.
    */
    uintptr_t page;
    localchar_t *result;
    alloc_for_size_t *alloc = &_STM_TL->alloc[size_class];
    size_t size = size_class * 8;

    /* reserve a fresh new page (XXX: from the end!) */
    page = stm_pages_reserve(1);

    assert(memset(real_address((object_t*)(page * 4096)), 0xdd, 4096));
    
    result = (localchar_t *)(page * 4096UL);
    alloc->start = (uintptr_t)result;
    alloc->stop = alloc->start + (4096 / size) * size;
    alloc->next = result + size;
    alloc->flag_partial_page = false;
    return result;
}

object_t *stm_big_small_alloc_old(size_t size, bool *is_small)
{
    /* may return uninitialized objects */
    object_t *result;
    size_t size_class = size / 8;
    assert(size_class >= 2);
    
    if (size_class >= LARGE_OBJECT_WORDS) {
        result = stm_large_malloc(size);
        *is_small = 0;
    } else {
        *is_small = 1;
        alloc_for_size_t *alloc = &_STM_TL->alloc[size_class];
        
        if ((uint16_t)((uintptr_t)alloc->next) == alloc->stop) {
            result = (object_t *)_stm_alloc_next_page(size_class);
        } else {
            result = (object_t *)alloc->next;
            alloc->next += size;
        }
    }
    return result;
}



void trace_if_young(object_t **pobj)
{
    /* takes a normal pointer to a thread-local pointer to an object */
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
    bool is_small;
    object_t *moved = stm_big_small_alloc_old(size, &is_small);

    memcpy((void*)real_address(moved),
           (void*)real_address(*pobj),
           size);

    /* object is not committed yet */
    moved->stm_flags |= GCFLAG_NOT_COMMITTED;
    if (is_small)              /* means, not allocated by large-malloc */
        moved->stm_flags |= GCFLAG_SMALL;
    assert(size == _stm_data_size((struct object_s*)REAL_ADDRESS(get_thread_base(0), moved)));
    LIST_APPEND(_STM_TL->uncommitted_objects, moved);
    
    (*pobj)->stm_flags |= GCFLAG_MOVED;
    *pforwarded = moved;
    *pobj = moved;
    
    LIST_APPEND(_STM_TL->old_objects_to_trace, moved);
}

void minor_collect()
{
    /* visit shadowstack & add to old_obj_to_trace */
    object_t **current = _STM_TL->shadow_stack;
    object_t **base = _STM_TL->shadow_stack_base;
    while (current-- != base) {
        trace_if_young(current);
    }
    
    /* visit old_obj_to_trace until empty */
    struct stm_list_s *old_objs = _STM_TL->old_objects_to_trace;
    while (!stm_list_is_empty(old_objs)) {
        object_t *item = stm_list_pop_item(old_objs);

        assert(!_stm_is_young(item));
        assert(!(item->stm_flags & GCFLAG_WRITE_BARRIER));
        
        /* re-add write-barrier */
        item->stm_flags |= GCFLAG_WRITE_BARRIER;
        
        stmcb_trace(real_address(item), trace_if_young);
        old_objs = _STM_TL->old_objects_to_trace;
    }

    /* clear nursery */
    localchar_t *nursery_base = (localchar_t*)(FIRST_NURSERY_PAGE * 4096);
    memset((void*)real_address((object_t*)nursery_base), 0x0,
           _STM_TL->nursery_current - nursery_base);
    _STM_TL->nursery_current = nursery_base;
}

void _stm_minor_collect()
{
    minor_collect();
}

localchar_t *collect_and_reserve(size_t size)
{
    _stm_start_safe_point(0);    /* don't release the COLLECT lock,
                                   that needs to be done afterwards if
                                   we want a major collection */
    minor_collect();
    _stm_stop_safe_point(0);

    /* XXX: if we_want_major_collect: acquire EXCLUSIVE & COLLECT lock
       and do it */

    localchar_t *current = _STM_TL->nursery_current;
    _STM_TL->nursery_current = current + size;
    return current;
}


object_t *stm_allocate(size_t size)
{
    _stm_start_safe_point(LOCK_COLLECT);
    /* all collections may happen here */
    _stm_stop_safe_point(LOCK_COLLECT);
    
    assert(_STM_TL->active);
    assert(size % 8 == 0);
    assert(16 <= size && size < NB_NURSERY_PAGES * 4096);//XXX

    localchar_t *current = _STM_TL->nursery_current;
    localchar_t *new_current = current + size;
    _STM_TL->nursery_current = new_current;
    assert((uintptr_t)new_current < (1L << 32));
    if ((uintptr_t)new_current > FIRST_AFTER_NURSERY_PAGE * 4096) {
        _STM_TL->nursery_current = current; /* reset for nursery-clearing in minor_collect!! */
        current = collect_and_reserve(size);
    }

    object_t *result = (object_t *)current;
    return result;
}


void push_uncommitted_to_other_threads()
{
    /* WE HAVE THE EXCLUSIVE LOCK HERE */
    
    struct stm_list_s *uncommitted = _STM_TL->uncommitted_objects;
    char *local_base = _STM_TL->thread_base;
    char *remote_base = get_thread_base(1 - _STM_TL->thread_num);
    
    STM_LIST_FOREACH(
        uncommitted,
        ({
            /* write-lock always cleared for these objects */
            uintptr_t lock_idx;
            assert(lock_idx = (((uintptr_t)item) >> 4) - READMARKER_START);
            assert(!write_locks[lock_idx]);

            /* remove the flag (they are now committed) */
            item->stm_flags &= ~GCFLAG_NOT_COMMITTED;

            _stm_move_object(item,
                REAL_ADDRESS(local_base, item),
                REAL_ADDRESS(remote_base, item));
        }));
}

void nursery_on_start()
{
    assert(stm_list_is_empty(_STM_TL->old_objects_to_trace));

    _STM_TL->old_shadow_stack = _STM_TL->shadow_stack;
}

void nursery_on_commit()
{
    /* DON'T do a minor_collect. This is already done in
       the caller (optimization) */
    /* minor_collect(); */
    
    /* uncommitted objects */
    push_uncommitted_to_other_threads();
    stm_list_clear(_STM_TL->uncommitted_objects);

    /* for small alloc classes, set the partial flag */
    long j;
    for (j = 2; j < LARGE_OBJECT_WORDS; j++) {
        alloc_for_size_t *alloc = &_STM_TL->alloc[j];
        uint16_t start = alloc->start;
        uint16_t cur = (uintptr_t)alloc->next;
        
        if (start == cur)
            continue;           /* page full -> will be replaced automatically */
        
        alloc->start = cur;     /* next transaction has different 'start' to
                                   reset in case of an abort */
        alloc->flag_partial_page = 1;
    }
}

void nursery_on_abort()
{
    /* reset shadowstack */
    _STM_TL->shadow_stack = _STM_TL->old_shadow_stack;

    /* clear old_objects_to_trace (they will have the WRITE_BARRIER flag
       set because the ones we care about are also in modified_objects) */
    stm_list_clear(_STM_TL->old_objects_to_trace);

    /* clear the nursery */
    localchar_t *nursery_base = (localchar_t*)(FIRST_NURSERY_PAGE * 4096);
    memset((void*)real_address((object_t*)nursery_base), 0x0,
           _STM_TL->nursery_current - nursery_base);
    _STM_TL->nursery_current = nursery_base;


    /* reset the alloc-pages to the state at the start of the transaction */
    long j;
    for (j = 2; j < LARGE_OBJECT_WORDS; j++) {
        alloc_for_size_t *alloc = &_STM_TL->alloc[j];
        uint16_t num_allocated = ((uintptr_t)alloc->next) - alloc->start;
        
        if (num_allocated) {
            /* forget about all non-committed objects */
            alloc->next -= num_allocated;
        }
    }
    
    /* free uncommitted objects */
    struct stm_list_s *uncommitted = _STM_TL->uncommitted_objects;
    
    STM_LIST_FOREACH(
        uncommitted,
        ({
            if (!(item->stm_flags & GCFLAG_SMALL))
                stm_large_free(item);
        }));
    
    stm_list_clear(uncommitted);
}



