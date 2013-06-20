#include "stmimpl.h"


#define LENGTH_SHADOW_STACK   163840


static __thread gcptr *stm_shadowstack;
static unsigned long stm_regular_length_limit = 10000;

void stm_set_transaction_length(long length_max)
{
    BecomeInevitable("set_transaction_length");
    if (length_max <= 0) {
        length_max = 1;
    }
    stm_regular_length_limit = length_max;
}

void stm_push_root(gcptr obj)
{
    *stm_shadowstack++ = obj;
}

gcptr stm_pop_root(void)
{
    return *--stm_shadowstack;
}

static void init_shadowstack(void)
{
    struct tx_descriptor *d = thread_descriptor;
    d->shadowstack = malloc(sizeof(gcptr) * LENGTH_SHADOW_STACK);
    if (!d->shadowstack) {
        fprintf(stderr, "out of memory: shadowstack\n");
        abort();
    }
    stm_shadowstack = d->shadowstack;
    d->shadowstack_end_ref = &stm_shadowstack;
    //stm_push_root(END_MARKER);
}

static void done_shadowstack(void)
{
    struct tx_descriptor *d = thread_descriptor;
    //gcptr x = stm_pop_root();
    //assert(x == END_MARKER);
    assert(stm_shadowstack == d->shadowstack);
    stm_shadowstack = NULL;
    free(d->shadowstack);
}

void stm_set_max_aborts(int max_aborts)
{
    struct tx_descriptor *d = thread_descriptor;
    d->max_aborts = max_aborts;
}

void stm_initialize(void)
{
    int r = DescriptorInit();
    assert(r == 1);
    stmgc_init_nursery();
    init_shadowstack();
    //stmgcpage_init_tls();
    BeginInevitableTransaction();
}

void stm_finalize(void)
{
    stmgc_minor_collect();   /* force everything out of the nursery */
    CommitTransaction();
    //stmgcpage_done_tls();
    done_shadowstack();
    stmgc_done_nursery();
    DescriptorDone();
}

gcptr stm_read_barrier(gcptr obj)
{
    //if (FXCACHE_AT(obj) == obj)
    //    fprintf(stderr, "read_barrier: in cache: %p\n", obj);

    /* XXX inline in the caller, optimize to get the smallest code */
    if (UNLIKELY((obj->h_revision != stm_private_rev_num) &&
                 (FXCACHE_AT(obj) != obj)))
        obj = stm_DirectReadBarrier(obj);
    return obj;
}

gcptr stm_write_barrier(gcptr obj)
{
    /* XXX inline in the caller */
    if (UNLIKELY((obj->h_revision != stm_private_rev_num) |
                 ((obj->h_tid & GCFLAG_WRITE_BARRIER) != 0)))
        obj = stm_WriteBarrier(obj);
    return obj;
}

/************************************************************/

static revision_t sync_required = 0;

void stm_perform_transaction(gcptr arg, int (*callback)(gcptr, int))
{   /* must save roots around this call */
    jmp_buf _jmpbuf;
    long volatile v_counter = 0;
    gcptr *volatile v_saved_value = stm_shadowstack;
    long volatile v_atomic;

    stm_push_root(arg);

    if (!(v_atomic = thread_descriptor->atomic))
        CommitTransaction();

#ifdef _GC_ON_CPYTHON
    volatile PyThreadState *v_ts = PyGILState_GetThisThreadState();
    volatile int v_recursion_depth = v_ts->recursion_depth;
#endif

    setjmp(_jmpbuf);

#ifdef _GC_ON_CPYTHON
    v_ts->recursion_depth = v_recursion_depth;
#endif

    /* After setjmp(), the local variables v_* are preserved because they
     * are volatile.  The other variables are only declared here. */
    struct tx_descriptor *d = thread_descriptor;
    long counter, result;
    counter = v_counter;
    d->atomic = v_atomic;
    stm_shadowstack = v_saved_value + 1;    /* skip the 'arg', pushed above */
    //    if (!d->atomic) {
    //        /* In non-atomic mode, we are now between two transactions.
    //           It means that in the next transaction's collections we know
    //           that we won't need to access the shadow stack beyond its
    //           current position.  So we add an end marker. */
    //        stm_push_root(END_MARKER);
    //    }

    do {
        v_counter = counter + 1;
        /* If counter==0, initialize 'reads_size_limit_nonatomic' from the
           configured length limit.  If counter>0, we did an abort, which
           has configured 'reads_size_limit_nonatomic' to a smaller value.
           When such a shortened transaction succeeds, the next one will
           see its length limit doubled, up to the maximum. */
        if (counter == 0) {
            unsigned long limit = d->reads_size_limit_nonatomic;
            if (limit != 0 && limit < (stm_regular_length_limit >> 1))
                limit = (limit << 1) | 1;
            else
                limit = stm_regular_length_limit;
            d->reads_size_limit_nonatomic = limit;
        }
        if (!d->atomic) {
            BeginTransaction(&_jmpbuf);
        }
        else {
            /* atomic transaction: a common case is that callback() returned
               even though we are atomic because we need a major GC.  For
               that case, release and require the rw lock here. */
            stm_possible_safe_point();
        }

        /* invoke the callback in the new transaction */
        arg = v_saved_value[0];
        result = callback(arg, counter);
        assert(stm_shadowstack == v_saved_value + 1);

        v_atomic = d->atomic;
        if (!d->atomic)
            CommitTransaction();

        counter = 0;
    }
    while (result > 0);  /* continue as long as callback() returned > 0 */

    if (d->atomic) {
        if (d->setjmp_buf == &_jmpbuf) {
            BecomeInevitable("perform_transaction left with atomic");
        }
    }
    else {
        BeginInevitableTransaction();
    }

    stm_pop_root();      /* pop the 'arg' */
    assert(stm_shadowstack == v_saved_value);
}

void stm_commit_transaction(void)
{   /* must save roots around this call */
    struct tx_descriptor *d = thread_descriptor;
    if (!d->atomic)
        CommitTransaction();
}

void stm_begin_inevitable_transaction(void)
{   /* must save roots around this call */
    struct tx_descriptor *d = thread_descriptor;
    if (!d->atomic)
        BeginInevitableTransaction();
}

int stm_in_transaction(void)
{
    struct tx_descriptor *d = thread_descriptor;
    return d && d->active;
}

/************************************************************/

/* a multi-reader, single-writer lock: transactions normally take a reader
   lock, so don't conflict with each other; when we need to do a global GC,
   we take a writer lock to "stop the world".  Note the initializer here,
   which should give the correct priority for stm_possible_safe_point(). */
static pthread_rwlock_t rwlock_shared =
    PTHREAD_RWLOCK_WRITER_NONRECURSIVE_INITIALIZER_NP;

static struct tx_descriptor *in_single_thread = NULL;  /* for debugging */

void stm_start_sharedlock(void)
{
    int err = pthread_rwlock_rdlock(&rwlock_shared);
    assert(err == 0);
    //assert(stmgc_nursery_hiding(thread_descriptor, 0));
}

void stm_stop_sharedlock(void)
{
    //assert(stmgc_nursery_hiding(thread_descriptor, 1));
    int err = pthread_rwlock_unlock(&rwlock_shared);
    assert(err == 0);
}

static void start_exclusivelock(void)
{
    int err = pthread_rwlock_wrlock(&rwlock_shared);
    assert(err == 0);
}

static void stop_exclusivelock(void)
{
    int err = pthread_rwlock_unlock(&rwlock_shared);
    assert(err == 0);
}

void stm_start_single_thread(void)
{
    /* Called by the GC, just after a minor collection, when we need to do
       a major collection.  When it returns, it acquired the "write lock"
       which prevents any other thread from running in a transaction.
       Warning, may block waiting for rwlock_in_transaction while another
       thread runs a major GC itself! */
    ACCESS_ONCE(sync_required) = 1;
    stm_stop_sharedlock();
    start_exclusivelock();
    ACCESS_ONCE(sync_required) = 0;

    assert(in_single_thread == NULL);
    in_single_thread = thread_descriptor;
    assert(in_single_thread != NULL);
}

void stm_stop_single_thread(void)
{
    /* Warning, may block waiting for rwlock_in_transaction while another
       thread runs a major GC */
    assert(in_single_thread == thread_descriptor);
    in_single_thread = NULL;

    stop_exclusivelock();
    stm_start_sharedlock();
}

void stm_possible_safe_point(void)
{
    if (!ACCESS_ONCE(sync_required))
        return;

    /* Warning, may block waiting for rwlock_in_transaction while another
       thread runs a major GC */
    struct tx_descriptor *d = thread_descriptor;
    assert(d->active);
    assert(in_single_thread != d);

    stm_stop_sharedlock();
    /* another thread should be waiting in start_exclusivelock(),
       which takes priority here */
    stm_start_sharedlock();
}

/************************************************************/

/***** Prebuilt roots, added in the list as the transaction that changed
       them commits *****/

struct GcPtrList stm_prebuilt_gcroots = {0};

void stm_add_prebuilt_root(gcptr obj)
{
    assert(obj->h_tid & GCFLAG_PREBUILT_ORIGINAL);
    gcptrlist_insert(&stm_prebuilt_gcroots, obj);
}

void stm_clear_between_tests(void)
{
    fprintf(stderr, "\n"
            "===============================================================\n"
            "========================[  START  ]============================\n"
            "===============================================================\n"
            "\n");
    gcptrlist_clear(&stm_prebuilt_gcroots);
}
