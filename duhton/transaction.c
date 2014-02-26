#include "duhton.h"
#include <pthread.h>
#include <unistd.h>

__thread stm_thread_local_t stm_thread_local;
static DuConsObject *du_pending_transactions;

void init_prebuilt_transaction_objects(void)
{
    assert(Du_None);   /* already created */

    du_pending_transactions = (DuConsObject *)
        _stm_allocate_old(sizeof(DuConsObject));
    du_pending_transactions->ob_base.type_id = DUTYPE_CONS;
    du_pending_transactions->car = NULL;
    du_pending_transactions->cdr = Du_None;

    _du_save1(du_pending_transactions);
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

__thread DuObject *stm_thread_local_obj = NULL;  /* XXX temp */


void Du_TransactionAdd(DuObject *code, DuObject *frame)
{
    DuObject *cell = DuCons_New(code, frame);
    DuObject *pending = stm_thread_local_obj;

    if (pending == NULL) {
        pending = Du_None;
    }
    pending = DuCons_New(cell, pending);
    stm_thread_local_obj = pending;
}

void Du_TransactionRun(void)
{
    if (stm_thread_local_obj == NULL)
        return;

    stm_start_inevitable_transaction(&stm_thread_local);

    DuConsObject *root = du_pending_transactions;
    _du_write1(root);
    root->cdr = stm_thread_local_obj;

    stm_commit_transaction();

    stm_thread_local_obj = NULL;

    run_all_threads();
}

/************************************************************/

static DuObject *next_cell(void)
{
    DuObject *pending = stm_thread_local_obj;

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

            stm_commit_transaction();

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
    stm_thread_local_obj = NULL;

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

    stm_commit_transaction();

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
    stm_jmpbuf_t here;
    stm_register_thread_local(&stm_thread_local);

    stm_thread_local_obj = NULL;

    while (1) {
        DuObject *cell = next_cell();
        if (cell == NULL)
            break;
        assert(stm_thread_local_obj == NULL);

        STM_START_TRANSACTION(&stm_thread_local, here);

        run_transaction(cell);

        _du_save1(stm_thread_local_obj);
        stm_collect(0);   /* hack.. */
        _du_restore1(stm_thread_local_obj);

        stm_commit_transaction();

    }

    stm_unregister_thread_local(&stm_thread_local);

    return NULL;
}
