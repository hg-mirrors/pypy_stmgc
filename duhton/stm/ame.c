#include <string.h>
#include <pthread.h>
#include "ame.h"
#include "lists.h"


/* XXX assumes that time never wraps around (in a 'long'), which may be
 * correct on 64-bit machines but not on 32-bit machines if the process
 * runs for long enough.
 */

#define IS_LOCKED(num)  ((num) < 0)
#define IS_LOCKED_OR_NEWER(num, max_age)                        \
    (((unsigned long)(num)) > ((unsigned long)(max_age)))


struct tx_descriptor {
    jmp_buf *setjmp_buf;
    int in_transaction;
    owner_version_t start_time;
    owner_version_t end_time;
    owner_version_t my_lock_word;
    struct OrecList reads;
    struct RedoLog redolog;   /* last item, because it's the biggest one */
};

/* global_timestamp contains in its lowest bit a flag equal to 1
   if there is an inevitable transaction running */
static volatile owner_version_t global_timestamp = 2;
static __thread struct tx_descriptor *thread_descriptor;


static void common_cleanup(struct tx_descriptor *d)
{
    d->reads.size = 0;
    redolog_clear(&d->redolog);
    d->in_transaction = 0;
}

static void tx_spinloop(void)
{
    spinloop();
}

static void tx_abort(void);

static int is_inevitable(struct tx_descriptor *d)
{
    assert(d->in_transaction);
    return d->setjmp_buf == NULL;
}


typedef struct {
    DuOBJECT_HEAD
    DuObject *ob_reference;
} DuContainerObject;

/*** run the redo log to commit a transaction, and release the locks */
static void tx_redo(struct tx_descriptor *d)
{
    owner_version_t newver = d->end_time;
    wlog_t *item;
    REDOLOG_LOOP_FORWARD(d->redolog, item)
    {
        DuObject *globalobj = item->addr;
        DuObject *localobj = item->val;
        long size = localobj->ob_type->dt_size;
        assert(size >= sizeof(DuObject));
        memcpy(((char *)globalobj) + sizeof(DuObject),
               ((char *)localobj) + sizeof(DuObject),
               size - sizeof(DuObject));
        CFENCE;
        globalobj->ob_version = newver;

        //int num = DuInt_AsInt(((DuContainerObject*)localobj)->ob_reference);
        //printf("COMMIT thread %lx: %p <- %p (%d), v. %ld\n",
        //        (long)pthread_self(), globalobj, localobj, num, newver);

    } REDOLOG_LOOP_END;
}

/*** on abort, release locks and restore the old version number. */
static void releaseAndRevertLocks(struct tx_descriptor *d)
{
  wlog_t *item;
  REDOLOG_LOOP_FORWARD(d->redolog, item)
  {
      if (item->p != -1) {
          DuObject* o = item->addr;
          o->ob_version = item->p;
      }
  } REDOLOG_LOOP_END;
}

/*** release locks and restore the old version number, ready to retry later */
static void releaseLocksForRetry(struct tx_descriptor *d)
{
    wlog_t *item;
    REDOLOG_LOOP_FORWARD(d->redolog, item)
    {
        if (item->p != -1) {
            DuObject* o = item->addr;
            o->ob_version = item->p;
            item->p = -1;
        }
    } REDOLOG_LOOP_END;
}

/*** lock all locations */
static void acquireLocks(struct tx_descriptor *d)
{
    wlog_t *item;
    // try to lock every location in the write set
    REDOLOG_LOOP_BACKWARD(d->redolog, item)
    {
        DuObject* o = item->addr;
        owner_version_t ovt;

    retry:
        ovt = o->ob_version;

        // if object not locked, lock it
        //
        // NB: if ovt > start time, we may introduce inconsistent reads.
        // Since most writes are also reads, we'll just abort under this
        // condition.  This can introduce false conflicts
        if (!IS_LOCKED_OR_NEWER(ovt, d->start_time)) {
            if (!bool_cas(&o->ob_version, ovt, d->my_lock_word)) {
                CFENCE;
                goto retry;
            }
            // save old version to item->p.  Now we hold the lock.
            item->p = ovt;
        }
        // else if the location is too recent...
        else if (!IS_LOCKED(ovt))
            tx_abort();
        // else it is locked: check it's not by me (no duplicates in redolog)
        else {
            assert(ovt != d->my_lock_word);
            // we can either abort or spinloop.  Because we are at the end of
            // the transaction we might try to spinloop, even though after the
            // lock is released the ovt will be very recent, possibly greater
            // than d->start_time.  It is necessary to spinloop in case we are
            // inevitable, so use that as a criteria.  Another solution to
            // avoid deadlocks would be to sort the order in which we take the
            // locks.
            if (is_inevitable(d))
                tx_spinloop();
            else
                tx_abort();
            goto retry;
        }
    } REDOLOG_LOOP_END;
}

/**
 * fast-path validation, assuming that I don't hold locks.
 */
static void validate_fast(struct tx_descriptor *d)
{
    int i;
    owner_version_t ovt;
    assert(!is_inevitable(d));
    for (i=0; i<d->reads.size; i++) {
     retry:
        ovt = d->reads.items[i]->ob_version;
        if (IS_LOCKED_OR_NEWER(ovt, d->start_time)) {
            // If locked, we wait until it becomes unlocked.  The chances are
            // that it will then have a very recent start_time, likely greater
            // than d->start_time, but it might still be better than always
            // aborting
            if (IS_LOCKED(ovt)) {
                tx_spinloop();
                goto retry;
            }
            else
                // abort if the timestamp is newer than my start time.  
                tx_abort();
        }
    }
}

/**
 * validate the read set by making sure that all orecs that we've read have
 * timestamps at least as old as our start time, unless we locked those orecs.
 */
static void validate(struct tx_descriptor *d)
{
    int i;
    owner_version_t ovt;
    assert(!is_inevitable(d));
    for (i=0; i<d->reads.size; i++) {
        ovt = d->reads.items[i]->ob_version;
        if (IS_LOCKED_OR_NEWER(ovt, d->start_time)) {
            if (!IS_LOCKED(ovt)) {
                // if unlocked and newer than start time, abort
                tx_abort();
            }
            else {
                // if locked and not by me, abort
                if (ovt != d->my_lock_word)
                    tx_abort();
            }
        }
    }
}

static void wait_end_inevitability(struct tx_descriptor *d)
{
    owner_version_t curts;
    releaseLocksForRetry(d);

    // We are going to wait until the other inevitable transaction
    // finishes.  XXX We could do better here: we could check if
    // committing 'd' would create a conflict for the other inevitable
    // thread 'd_inev' or not.  It requires peeking in 'd_inev' from this
    // thread (which we never do so far) in order to do something like
    // 'validate_fast(d_inev); d_inev->start_time = updated;'

    while ((curts = global_timestamp) & 1) {
        // while we're about to wait anyway, we can do a validate_fast
        if (d->start_time < curts - 1) {
            validate_fast(d);
            d->start_time = curts - 1;
        }
        tx_spinloop();
    }
    acquireLocks(d);
}

static void commitInevitableTransaction(struct tx_descriptor *d)
{
    owner_version_t ts;

    // no-one else can modify global_timestamp if I'm inevitable
    ts = global_timestamp;
    assert(ts & 1);
    global_timestamp = ts + 1;
    d->end_time = ts + 1;
    assert(d->end_time == (d->start_time + 2));

    // run the redo log, and release the locks
    tx_redo(d);
}

static void tx_abort(void)
{
    struct tx_descriptor *d = thread_descriptor;
    assert(!is_inevitable(d));
    // release the locks and restore version numbers
    releaseAndRevertLocks(d);
    // reset all lists
    common_cleanup(d);

    tx_spinloop();
    longjmp(*d->setjmp_buf, 1);
}


static inline owner_version_t prepare_read(struct tx_descriptor *d,
                                           DuObject *glob)
{
    owner_version_t ovt;
 retry:
    ovt = glob->ob_version;
    if (IS_LOCKED_OR_NEWER(ovt, d->start_time)) {
        if (IS_LOCKED(ovt)) {
            tx_spinloop();
            goto retry;
        }
        /* else this location is too new, scale forward */
        owner_version_t newts = global_timestamp & ~1;
        validate_fast(d);
        d->start_time = newts;
    }
    return ovt;
}

DuObject *_Du_AME_read_from_global(DuObject *glob, owner_version_t *vers_out)
{
    struct tx_descriptor *d = thread_descriptor;
    wlog_t *found;
    REDOLOG_FIND(d->redolog, glob, found, goto not_found);
    DuObject *localobj = found->val;
    assert(!Du_AME_GLOBAL(localobj));
    return localobj;

 not_found:
    *vers_out = prepare_read(d, glob);
    return glob;
}

void _Du_AME_oreclist_insert(DuObject *glob)
{
    struct tx_descriptor *d = thread_descriptor;
    oreclist_insert(&d->reads, glob);
}

DuObject *_Du_AME_writebarrier(DuObject *glob)
{
    struct tx_descriptor *d = thread_descriptor;
    if (!d->in_transaction)
        return glob;

    DuObject *localobj;
    wlog_t* found;
    REDOLOG_FIND(d->redolog, glob, found, goto not_found);
    localobj = found->val;
    assert(!Du_AME_GLOBAL(localobj));
    return localobj;

 not_found:;
    /* We need to really make a local copy */
    owner_version_t version;
    int size = glob->ob_type->dt_size;
    localobj = malloc(size);
    assert(localobj != NULL);

    do {
        version = prepare_read(d, glob);
        CFENCE;
        /* Initialize the copy by doing an stm raw copy of the bytes */
        memcpy((char *)localobj, (char *)glob, size);
        CFENCE;
        /* Check the copy for validity */
    } while (glob->ob_version != version);

    /* Initialize the copy's refcount to be a valid local object */
    localobj->ob_refcnt = 42;   /* XXX */
    /* Set the ob_debug_prev field to NULL and the ob_debug_next field to
       point to the global object */
    localobj->ob_debug_prev = NULL;
    localobj->ob_debug_next = glob;
    /* Ask for additional type-specific copy */
    if (localobj->ob_type->dt_ame_copy)
        localobj->ob_type->dt_ame_copy(localobj);
    /* Register the object as a valid copy */
    redolog_insert(&d->redolog, glob, localobj);
    return localobj;
}

DuObject *_Du_AME_getlocal(DuObject *glob)
{
    struct tx_descriptor *d = thread_descriptor;
    DuObject *localobj;
    wlog_t* found;
    REDOLOG_FIND(d->redolog, glob, found, return glob);
    localobj = found->val;
    assert(!Du_AME_GLOBAL(localobj));
    return localobj;
}

void _Du_AME_InitThreadDescriptor(void)
{
    assert(thread_descriptor == NULL);
    struct tx_descriptor *d = malloc(sizeof(struct tx_descriptor));
    memset(d, 0, sizeof(struct tx_descriptor));

    /* initialize 'my_lock_word' to be a unique negative number */
    d->my_lock_word = (owner_version_t)d;
    if (!IS_LOCKED(d->my_lock_word))
        d->my_lock_word = ~d->my_lock_word;
    assert(IS_LOCKED(d->my_lock_word));

    common_cleanup(d);
    thread_descriptor = d;
}

void _Du_AME_FiniThreadDescriptor(void)
{
    struct tx_descriptor *d = thread_descriptor;
    assert(d);
    thread_descriptor = NULL;
    free(d);
}

void _Du_AME_StartTransaction(jmp_buf *setjmp_buf)
{
    struct tx_descriptor *d = thread_descriptor;
    assert(!d->in_transaction);
    d->setjmp_buf = setjmp_buf;
    d->in_transaction = 1;
    d->start_time = global_timestamp & ~1;
}

void _Du_AME_CommitTransaction(void)
{
    struct tx_descriptor *d = thread_descriptor;
    assert(d->in_transaction);

    // if I don't have writes, I'm committed
    if (!redolog_any_entry(&d->redolog)) {
        if (is_inevitable(d)) {
            owner_version_t ts = global_timestamp;
            assert(ts & 1);
            global_timestamp = ts - 1;
        }
        common_cleanup(d);
        return;
    }

    //printf("COMMIT thread %lx: START\n",
    //        (long)pthread_self());

    // acquire locks
    acquireLocks(d);

    if (is_inevitable(d)) {
        commitInevitableTransaction(d);
    }
    else {
        while (1) {
            owner_version_t expected = global_timestamp;
            if (expected & 1) {
                // wait until it is done.  hopefully we can then proceed
                // without conflicts.
                wait_end_inevitability(d);
                continue;
            }
            if (bool_cas(&global_timestamp, expected, expected + 2)) {
                d->end_time = expected + 2;
                break;
            }
        }

        // validate (but skip validation if nobody else committed)
        if (d->end_time != (d->start_time + 2))
            validate(d);

        // run the redo log, and release the locks
        tx_redo(d);
    }

    //printf("COMMIT thread %lx: DONE\n",
    //        (long)pthread_self());

    common_cleanup(d);
}

void Du_AME_TryInevitable(void)
{
    struct tx_descriptor *d = thread_descriptor;
    if (!d->in_transaction || is_inevitable(d))
        return;

    while (1) {
        owner_version_t curtime = global_timestamp;
        if (d->start_time != (curtime & ~1)) {
            /* scale forward */
            validate_fast(d);
            d->start_time = curtime & ~1;
        }
        if (curtime & 1) { /* there is, or was, already an inevitable thread */
            /* should we spinloop here, or abort (and likely come back
               in try_inevitable() very soon)?  unclear.  For now
               let's try to spinloop. */
            tx_spinloop();
            continue;
        }
        if (bool_cas(&global_timestamp, curtime, curtime + 1))
            break;
    }
    d->setjmp_buf = NULL;   /* inevitable from now on */
}
