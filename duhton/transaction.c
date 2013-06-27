#include "duhton.h"
#include <pthread.h>
#include <unistd.h>

#define NUM_THREADS  4


static DuConsObject du_pending_transactions = {
    DuOBJECT_HEAD_INIT(DUTYPE_CONS),
    NULL,
    Du_None,
};

static pthread_mutex_t mutex_sleep = PTHREAD_MUTEX_INITIALIZER;
static int thread_sleeping = 0;

static void *run_thread(void *);   /* forward */

static void run_all_threads(void)
{
    int i;
    pthread_t th[NUM_THREADS];

    for (i = 0; i < NUM_THREADS; i++) {
        int status = pthread_create(&th[i], NULL, run_thread, NULL);
        if (status != 0)
            stm_fatalerror("status != 0\n");
    }
    for (i = 0; i < NUM_THREADS; i++) {
        pthread_join(th[i], NULL);
    }
}

/************************************************************/

void Du_TransactionAdd(DuObject *code, DuObject *frame)
{
    DuObject *cell = DuCons_New(code, frame);
    DuObject *pending = (DuObject *)stm_thread_local_obj;

    if (pending == NULL) {
        pending = Du_None;
    }
    pending = DuCons_New(cell, pending);
    stm_thread_local_obj = (gcptr)pending;
}

void Du_TransactionRun(void)
{
    if (stm_thread_local_obj == NULL)
        return;

    DuConsObject *root = &du_pending_transactions;
    _du_write1(root);
    root->cdr = stm_thread_local_obj;

    stm_commit_transaction();
    run_all_threads();
    stm_begin_inevitable_transaction();
}

/************************************************************/

static DuObject *next_cell(void)
{
    DuObject *pending = (DuObject *)stm_thread_local_obj;

    if (pending == NULL) {
        /* fish from the global list of pending transactions */
        DuConsObject *root;

      restart:
        root = &du_pending_transactions;
        _du_read1(root);

        if (root->cdr != Du_None) {
            DuObject *cell = root->cdr;
            _du_write1(root);

            _du_read1(cell);
            DuObject *result = _DuCons_CAR(cell);
            root->cdr = _DuCons_NEXT(cell);

            return result;
        }
        else {
            /* nothing to do, wait */
            thread_sleeping++;
            if (thread_sleeping == NUM_THREADS) {
                pthread_mutex_unlock(&mutex_sleep);
            }
            stm_commit_transaction();
            pthread_mutex_lock(&mutex_sleep);
            stm_begin_inevitable_transaction();
            if (thread_sleeping == NUM_THREADS) {
                pthread_mutex_unlock(&mutex_sleep);
                return NULL;
            }
            thread_sleeping--;
            goto restart;
        }
    }

    /* we have at least one thread-local transaction pending */
    _du_read1(pending);
    DuObject *result = _DuCons_CAR(pending);
    DuObject *next = _DuCons_NEXT(pending);

    if (next != Du_None) {
        /* we have more than one: add the others to the global list */
        assert(!"XXX");
        abort();
    }

    return result;
}

int run_transaction(gcptr cell, int retry_counter)
{
    DuObject *code  = DuCons_Car(cell);
    DuObject *frame = DuCons_Cdr(cell);
    Du_Progn(code, frame);
    return 0;
}

void *run_thread(void *ignored)
{
    stm_initialize();

    while (1) {
        /* we are inevitable here */
        DuObject *cell = next_cell();
        if (cell == NULL)
            break;
        stm_perform_transaction(cell, run_transaction);
    }

    stm_finalize();
    return NULL;
}
