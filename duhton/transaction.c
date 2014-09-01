#include "duhton.h"
#include <pthread.h>
#include <unistd.h>

__thread stm_thread_local_t stm_thread_local;
static DuConsObject *du_pending_transactions;

void init_prebuilt_transaction_objects(void)
{
    static DuConsObject pending = { {.type_id = DUTYPE_CONS} };
    du_pending_transactions = INIT_PREBUILT(&pending);

    assert(Du_None);   /* already created */
    du_pending_transactions->cdr = Du_None;
};

static pthread_mutex_t mutex_sleep = PTHREAD_MUTEX_INITIALIZER;
static int volatile thread_sleeping = 0;

static void *run_thread(void *);   /* forward */

static void run_all_threads(void)
{
    thread_sleeping = 0;
    int i;
    for (i = 0; i < all_threads_count; i++) {
        int status = pthread_create(&all_threads[i], NULL, run_thread,
                                    (void *)(uintptr_t)i);
        if (status != 0) {
            fprintf(stderr, "status != 0\n");
            abort();
        }
    }
    for (i = 0; i < all_threads_count; i++) {
        pthread_join(all_threads[i], NULL);
        all_threads[i] = (pthread_t)NULL;
    }
}

/************************************************************/

#define TLOBJ   (*((DuObject **)(&stm_thread_local.thread_local_obj)))

void Du_TransactionAdd(DuObject *code, DuObject *frame)
{
    DuObject *cell = DuCons_New(code, frame);
    DuObject *pending = TLOBJ;

    if (pending == NULL) {
        pending = Du_None;
    }
    pending = DuCons_New(cell, pending);
    TLOBJ = pending;
}

void Du_TransactionRun(void)
{
    if (TLOBJ == NULL)
        return;

    stm_start_inevitable_transaction(&stm_thread_local);

    DuConsObject *root = du_pending_transactions;
    _du_write1(root);
    root->cdr = TLOBJ;

    TLOBJ = NULL;
    stm_commit_transaction();

    run_all_threads();
}

/************************************************************/

static DuObject *next_cell(void)
{
    DuObject *pending = TLOBJ;

    if (pending == NULL) {
        /* fish from the global list of pending transactions */
        DuConsObject *root;

      restart:
        /* this code is critical enough so that we want it to
           be serialized perfectly using inevitable transactions */
        stm_start_inevitable_transaction(&stm_thread_local);

        root = du_pending_transactions;
        _du_read1(root);        /* not immutable... */

        if (root->cdr != Du_None) {
            DuObject *cell = root->cdr;
            _du_write1(root);

            /* _du_read1(cell); IMMUTABLE */
            DuObject *result = _DuCons_CAR(cell);
            root->cdr = _DuCons_NEXT(cell);

            return result;
        }
        else {
            stm_commit_transaction();

            /* nothing to do, wait */
            int ts = __sync_add_and_fetch(&thread_sleeping, 1);
            if (ts == all_threads_count) {
                pthread_mutex_unlock(&mutex_sleep);
            }
            pthread_mutex_lock(&mutex_sleep);

            while (1) {
                ts = thread_sleeping;
                if (ts == all_threads_count) {
                    pthread_mutex_unlock(&mutex_sleep);
                    return NULL;
                }
                assert(ts > 0);
                if (__sync_bool_compare_and_swap(&thread_sleeping, ts, ts - 1))
                    break;
            }
            goto restart;
        }
    }

    /* we have at least one thread-local transaction pending */
    TLOBJ = NULL;

    stm_start_inevitable_transaction(&stm_thread_local);

    /* _du_read1(pending); IMMUTABLE */
    DuObject *result = _DuCons_CAR(pending);
    DuObject *next = _DuCons_NEXT(pending);

    if (next != Du_None) {
        /* we have more than one: add the others to the global list */
        DuObject *tail = next;

        while (1) {
            /* _du_read1(tail); IMMUTABLE */
            DuObject *tailnext = ((DuConsObject *)tail)->cdr;
            if (tailnext == Du_None)
                break;
            tail = tailnext;
        }

        DuConsObject * root = du_pending_transactions;
        _du_write1(tail);
        _du_write1(root);
        ((DuConsObject *)tail)->cdr = root->cdr;
        root->cdr = next;
    }

    return result;
}

void run_transaction(DuObject *cell)
{
    DuObject *code  = DuCons_Car(cell);
    DuObject *frame = DuCons_Cdr(cell);
    Du_Progn(code, frame);
}

void *run_thread(void *thread_id)
{
    rewind_jmp_buf rjbuf;
    stm_register_thread_local(&stm_thread_local);
    stm_rewind_jmp_enterframe(&stm_thread_local, &rjbuf);

    TLOBJ = NULL;

    while (1) {
        DuObject *cell = next_cell();

        if (cell == NULL)       /* no transaction */
            break;
        assert(TLOBJ == NULL);

        TLOBJ = cell;
        stm_commit_transaction(); /* inevitable */
        stm_start_transaction(&stm_thread_local);
        cell = TLOBJ;
        TLOBJ = NULL;

        run_transaction(cell);

        stm_commit_transaction();

    }

    stm_flush_timing(&stm_thread_local, 1);
    stm_rewind_jmp_leaveframe(&stm_thread_local, &rjbuf);
    stm_unregister_thread_local(&stm_thread_local);

    return NULL;
}
