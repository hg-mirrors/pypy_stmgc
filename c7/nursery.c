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
    assert(_STM_TL->running_transaction);
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
    object_t *moved = stm_large_malloc(size);

    memcpy((void*)real_address(moved),
           (void*)real_address(*pobj),
           size);

    /* object is not committed yet */
    moved->stm_flags |= GCFLAG_NOT_COMMITTED;
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
    _stm_start_safe_point();
    minor_collect();
    _stm_stop_safe_point();

    localchar_t *current = _STM_TL->nursery_current;
    _STM_TL->nursery_current = current + size;
    return current;
}


object_t *stm_allocate(size_t size)
{
    _stm_start_safe_point();
    _stm_stop_safe_point();
    assert(_STM_TL->running_transaction);
    assert(size % 8 == 0);
    assert(16 <= size && size < NB_NURSERY_PAGES * 4096);//XXX

    localchar_t *current = _STM_TL->nursery_current;
    localchar_t *new_current = current + size;
    _STM_TL->nursery_current = new_current;
    assert((uintptr_t)new_current < (1L << 32));
    if ((uintptr_t)new_current > FIRST_AFTER_NURSERY_PAGE * 4096) {
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
}

void nursery_on_abort()
{
    
    /* clear old_objects_to_trace (they will have the WRITE_BARRIER flag
       set because the ones we care about are also in modified_objects) */
    stm_list_clear(_STM_TL->old_objects_to_trace);

    /* clear the nursery */
    localchar_t *nursery_base = (localchar_t*)(FIRST_NURSERY_PAGE * 4096);
    memset((void*)real_address((object_t*)nursery_base), 0x0,
           _STM_TL->nursery_current - nursery_base);
    _STM_TL->nursery_current = nursery_base;


    /* free uncommitted objects */
    struct stm_list_s *uncommitted = _STM_TL->uncommitted_objects;
    
    STM_LIST_FOREACH(
        uncommitted,
        ({
            stm_large_free(item);
        }));
    
    stm_list_clear(uncommitted);
}



