#include "stmsync.h"
#include "core.h"
#include "reader_writer_lock.h"
#include <assert.h>
#include <string.h>


/* a multi-reader, single-writer lock: transactions normally take a reader
   lock, so don't conflict with each other; when we need to do a global GC,
   we take a writer lock to "stop the world". */

rwticket rw_shared_lock;        /* the "GIL" */

void _stm_reset_shared_lock()
{
    assert(!rwticket_wrtrylock(&rw_shared_lock));
    assert(!rwticket_wrunlock(&rw_shared_lock));

    memset(&rw_shared_lock, 0, sizeof(rwticket));
}

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
