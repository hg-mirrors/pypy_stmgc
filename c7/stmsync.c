#include <assert.h>
#include <string.h>
#include <unistd.h>


#include "stmsync.h"
#include "core.h"
#include "reader_writer_lock.h"


/* a multi-reader, single-writer lock: transactions normally take a reader
   lock, so don't conflict with each other; when we need to do a global GC,
   we take a writer lock to "stop the world". */

rwticket rw_shared_lock;        /* the "GIL" */
rwticket rw_collection_lock;    /* for major collections */


void _stm_reset_shared_lock()
{
    assert(!rwticket_wrtrylock(&rw_shared_lock));
    assert(!rwticket_wrunlock(&rw_shared_lock));

    memset(&rw_shared_lock, 0, sizeof(rwticket));

    assert(!rwticket_wrtrylock(&rw_collection_lock));
    assert(!rwticket_wrunlock(&rw_collection_lock));

    memset(&rw_collection_lock, 0, sizeof(rwticket));
}

void stm_acquire_collection_lock()
{
    /* we must have the exclusive lock here and
       not the colletion lock!! */
    /* XXX: for more than 2 threads, need a way
       to signal other threads with need_major_collect
       so that they don't leave COLLECT-safe-points
       when this flag is set. Otherwise we simply
       wait arbitrarily long until all threads reach
       COLLECT-safe-points by chance at the same time. */
    while (1) {
        if (!rwticket_wrtrylock(&rw_collection_lock))
            break;              /* acquired! */
        
        stm_stop_exclusive_lock();
        usleep(1);
        stm_start_exclusive_lock();
        if (_STM_TL->need_abort) {
            stm_stop_exclusive_lock();
            stm_start_shared_lock();
            stm_abort_transaction();
        }
    }
}

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
   -> release the exclusive lock and also the collect-read-lock */
void _stm_start_safe_point(uint8_t flags)
{
    if (flags & LOCK_EXCLUSIVE)
        stm_stop_exclusive_lock();
    else
        stm_stop_shared_lock();
    
    if (flags & LOCK_COLLECT)
        rwticket_rdunlock(&rw_collection_lock);
}

/*
  _stm_stop_safe_point(LOCK_COLLECT|LOCK_EXCLUSIVE);
  -> reacquire the collect-read-lock and the exclusive lock
 */
void _stm_stop_safe_point(uint8_t flags)
{
    if (flags & LOCK_EXCLUSIVE)
        stm_start_exclusive_lock();
    else
        stm_start_shared_lock();
    
    if (!(flags & LOCK_COLLECT)) { /* if we released the collection lock */
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
            if (flags & LOCK_COLLECT)
                rwticket_rdlock(&rw_collection_lock);
            stm_abort_transaction();
        } else {
            if (flags & LOCK_COLLECT)
                rwticket_rdlock(&rw_collection_lock);
            stm_abort_transaction();
        }
    }
}



