
/* Modification locks protect from concurrent modification of
   'modified_old_objects', page-revision-changes, ...

   Modification locks are used to prevent copying from a segment
   where either the revision of some pages is inconsistent with the
   rest, or the modified_old_objects list is being modified (bk_copys).

   Lock ordering: acquire privatization lock around acquiring a set
   of modification locks!
*/

typedef struct {
    pthread_rwlock_t lock;
#ifndef NDEBUG
    volatile bool write_locked;
#endif
} modification_lock_t __attribute__((aligned(64)));

static modification_lock_t _modlocks[NB_SEGMENTS - 1];

#define PTHREAD_CHK(call)  do {                 \
    int _err = (call);                          \
    if (UNLIKELY(_err != 0))                    \
        stm_fatalerror(#call ": %d", _err);     \
} while (0)


static void setup_modification_locks(void)
{
    /* this is also called from forksupport_child() */
    int i;
    for (i = 1; i < NB_SEGMENTS; i++)
        PTHREAD_CHK(pthread_rwlock_init(&_modlocks[i - 1].lock, NULL));
}

static void teardown_modification_locks(void)
{
    int i;
    for (i = 1; i < NB_SEGMENTS; i++)
        pthread_rwlock_destroy(&_modlocks[i - 1].lock);
    memset(_modlocks, 0, sizeof(_modlocks));
}


static inline void acquire_modification_lock_wr(int segnum)
{
    PTHREAD_CHK(pthread_rwlock_wrlock(&_modlocks[segnum - 1].lock));
#ifndef NDEBUG
    assert(!_modlocks[segnum - 1].write_locked);
    _modlocks[segnum - 1].write_locked = true;
#endif
}

static inline void release_modification_lock_wr(int segnum)
{
#ifndef NDEBUG
    assert(_modlocks[segnum - 1].write_locked);
    _modlocks[segnum - 1].write_locked = false;
#endif
    PTHREAD_CHK(pthread_rwlock_unlock(&_modlocks[segnum - 1].lock));
}

static void acquire_modification_lock_set(uint64_t readset, int write)
{
    /* acquire the modification lock in 'read' mode for all segments
       in 'readset', plus the modification lock in 'write' mode for
       the segment number 'write'.
    */
    assert(NB_SEGMENTS <= 64);
    OPT_ASSERT(readset < (1 << NB_SEGMENTS));
    assert((readset & 1) == 0);       /* segment numbers normally start at 1 */
    assert(0 <= write && write < NB_SEGMENTS);     /* use 0 to mean "nobody" */

    /* acquire locks in global order */
    readset |= (1UL << write);
    int i;
    for (i = 1; i < NB_SEGMENTS; i++) {
        if ((readset & (1UL << i)) == 0)
            continue;
        if (i == write) {
            acquire_modification_lock_wr(write);
        }
        else {
            PTHREAD_CHK(pthread_rwlock_rdlock(&_modlocks[i - 1].lock));
        }
    }
}

static void release_modification_lock_set(uint64_t readset, int write)
{
    assert(NB_SEGMENTS <= 64);
    OPT_ASSERT(readset < (1 << NB_SEGMENTS));

    /* release lock order does not matter; prefer early release of
       the write lock */
    if (write > 0) {
        release_modification_lock_wr(write);
        readset &= ~(1UL << write);
    }
    int i;
    for (i = 1; i < NB_SEGMENTS; i++) {
        if ((readset & (1UL << i)) == 0)
            continue;
        PTHREAD_CHK(pthread_rwlock_unlock(&_modlocks[i - 1].lock));
    }
}

#ifndef NDEBUG
static bool modification_lock_check_rdlock(int segnum)
{
    assert(segnum > 0);
    if (_modlocks[segnum - 1].write_locked)
        return false;
    if (pthread_rwlock_trywrlock(&_modlocks[segnum - 1].lock) == 0) {
        pthread_rwlock_unlock(&_modlocks[segnum - 1].lock);
        return false;
    }
    return true;
}
static bool modification_lock_check_wrlock(int segnum)
{
    return segnum == 0 || _modlocks[segnum - 1].write_locked;
}
#endif






/* The 'inevitable_perf_lock' idea is a read-write lock that should be
   acquired in these situations:

   - for writing when there is an inevitable transaction running.
     More precisely, just around the time the head of the commit log
     is set to INEV_RUNNING;

   - for reading, shortly, when we're trying to commit a
     non-inevitable transaction.

   This has the following effects:

   - when we want to commit a non-inevitable transaction, if there is
     an inevitable transaction running, then we block;

   - when we want to become inevitable, we first wait for the other
     threads that are about to commit.

   We cannot use a pthread_rwlock, because:

   - in detach.c, we might commit a different thread's inevitable
     transaction.  The pthread_rwlock does not give a way to release
     the write lock acquired by a different thread;

   - we expect a precise priority, subtly different from Linux's.
     Writers should always have less priority than readers, because
     in stm_become_inevitable(), it is always a good idea to let
     other transactions that want to commit go ahead, before we
     turn inevitable.
*/


/* The 'inevitable_perf_lock' idea is a read-write lock that should be
   acquired in these situations:

   - for writing when there is an inevitable transaction running.
     More precisely, just around the time the head of the commit log
     is set to INEV_RUNNING;

   - for reading, shortly, when we're trying to commit a
     non-inevitable transaction.

   This has the following effects:

   - when we want to commit a non-inevitable transaction, if there is
     an inevitable transaction running, then we block;

   - when we want to become inevitable, we first wait for the other
     threads that are about to commit.

   We cannot use a pthread_rwlock, because:

   - in detach.c, we might commit a different thread's inevitable
     transaction.  The pthread_rwlock does not give a way to release
     the write lock acquired by a different thread;

   - we expect a precise priority, subtly different from Linux's.
     Writers should always have less priority than readers, because
     in stm_become_inevitable(), it is always a good idea to let
     other transactions that want to commit go ahead, before we
     turn inevitable.
*/
