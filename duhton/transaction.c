#include "duhton.h"
#include <pthread.h>
#include <unistd.h>


static DuConsObject *du_pending_transactions;

void init_prebuilt_transaction_objects(void)
{
    assert(Du_None);   /* already created */

    du_pending_transactions = (DuConsObject *)
        stm_allocate_prebuilt(sizeof(DuConsObject));
    du_pending_transactions->ob_base.type_id = DUTYPE_CONS;
    du_pending_transactions->car = NULL;
    du_pending_transactions->cdr = Du_None;
};

static pthread_mutex_t mutex_sleep = PTHREAD_MUTEX_INITIALIZER;
static int volatile thread_sleeping = 0;

static void *run_thread(void *);   /* forward */

static void run_all_threads(void)
{
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

    stm_start_transaction(NULL);
    DuConsObject *root = du_pending_transactions;
    _du_write1(root);
    root->cdr = stm_thread_local_obj;
    stm_stop_transaction();
    stm_thread_local_obj = NULL;

    run_all_threads();
}

/************************************************************/

static DuObject *next_cell(void)
{
    DuObject *pending = stm_thread_local_obj;
    jmpbufptr_t here;

    if (pending == NULL) {
        /* fish from the global list of pending transactions */
        DuConsObject *root;

        while (__builtin_setjmp(here) == 1) { }
      restart:
        stm_start_transaction(&here);

        root = du_pending_transactions;
        _du_read1(root);

        if (root->cdr != Du_None) {
            DuObject *cell = root->cdr;
            _du_write1(root);

            _du_read1(cell);
            DuObject *result = _DuCons_CAR(cell);
            root->cdr = _DuCons_NEXT(cell);

            stm_stop_transaction();

            return result;
        }
        else {
            stm_stop_transaction();

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

    while (__builtin_setjmp(here) == 1) { }
    stm_start_transaction(&here);

    _du_read1(pending);
    DuObject *result = _DuCons_CAR(pending);
    DuObject *next = _DuCons_NEXT(pending);

    if (next != Du_None) {
        /* we have more than one: add the others to the global list */
        DuObject *tail = next;

        while (1) {
            _du_read1(tail);
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

    stm_stop_transaction();

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
    jmpbufptr_t here;
    int thread_num = (uintptr_t)thread_id;
    _stm_restore_local_state(thread_num);
    stm_thread_local_obj = NULL;

    while (1) {
        DuObject *cell = next_cell();
        if (cell == NULL)
            break;
        assert(stm_thread_local_obj == NULL);

        while (__builtin_setjmp(here) == 1) { }
        stm_start_transaction(&here);
        run_transaction(cell);
        stm_stop_transaction();
    }

    return NULL;
}
