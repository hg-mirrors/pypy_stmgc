#include <pthread.h>
#include <assert.h>
#include "../duhton.h"
#include "fifo.h"


#define NUMTHREADS    2


static int num_waiting_threads;
static int finished;
static pthread_mutex_t ll_state = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t ll_no_tasks_pending = PTHREAD_MUTEX_INITIALIZER;
static fifo_t global_list_pending = FIFO_INITIALIZER;
static __thread fifo_t *local_list_pending = &global_list_pending;

static void lock(pthread_mutex_t *mutex)
{
    if (pthread_mutex_lock(mutex) != 0)
        Du_FatalError("lock() failed??");
}

static void unlock(pthread_mutex_t *mutex)
{
    if (pthread_mutex_unlock(mutex) != 0)
        Du_FatalError("unlock() failed: the lock is not acquired?");
}

static int is_locked(pthread_mutex_t *mutex)
{
    /* Test a lock for debugging. */
    if (pthread_mutex_trylock(mutex) != 0)
        return 1;
    unlock(mutex);
    return 0;
}

void Du_TransactionAdd(DuObject *code, DuObject *frame)
{
    fifonode_t *fnode = malloc(sizeof(fifonode_t));
    fnode->fn_code = code;   Du_INCREF(code);
    fnode->fn_frame = frame; Du_INCREF(frame);
    fifo_append(local_list_pending, fnode);
}


//static pthread_mutex_t tmp_GIL = PTHREAD_MUTEX_INITIALIZER;

static void execute_fifo_node(fifonode_t *fnode)
{
    jmp_buf env;
    assert(fifo_is_empty(local_list_pending));
    //lock(&tmp_GIL);
    if (setjmp(env) == 0) {
        _Du_AME_StartTransaction(&env);
        _Du_InitializeObjects();
        DuObject *framecopy = DuFrame_Copy(fnode->fn_frame);
        DuObject *res = Du_Progn(fnode->fn_code, framecopy);
        Du_DECREF(res);
        Du_DECREF(framecopy);
        Du_DECREF(fnode->fn_frame);
        Du_DECREF(fnode->fn_code);
        _Du_MakeImmortal();
        _Du_AME_CommitTransaction();
        free(fnode);
    }
    else {    /* transaction aborted, re-schedule fnode for later */
        fifo_t *local_list = local_list_pending;
        fifo_init(local_list);
        fifo_append(local_list, fnode);
    }
    //unlock(&tmp_GIL);
}

static void _add_list(fifo_t *new_pending_list)
{
    if (fifo_is_empty(new_pending_list))
        return;
    int was_empty = fifo_is_empty(&global_list_pending);
    fifo_steal(&global_list_pending, new_pending_list);
    if (was_empty)
        unlock(&ll_no_tasks_pending);
}

static void *_run_thread(void *arg)
{
    lock(&ll_state);
    _Du_AME_InitThreadDescriptor();
    fifo_t my_transactions_pending = FIFO_INITIALIZER;
    local_list_pending = &my_transactions_pending;

    while (1) {
        if (fifo_is_empty(&global_list_pending)) {
            assert(is_locked(&ll_no_tasks_pending));
            num_waiting_threads++;
            if (num_waiting_threads == NUMTHREADS) {
                finished = 1;
                unlock(&ll_no_tasks_pending);
            }
            unlock(&ll_state);

            lock(&ll_no_tasks_pending);
            unlock(&ll_no_tasks_pending);

            lock(&ll_state);
            num_waiting_threads--;
            if (finished)
                break;
        }
        else {
            fifonode_t *pending = fifo_pop_left(&global_list_pending);
            if (fifo_is_empty(&global_list_pending))
                lock(&ll_no_tasks_pending);
            unlock(&ll_state);

            while (1) {
                execute_fifo_node(pending);
                /* for now, always break out of this loop, unless
                   'my_transactions_pending' contains precisely one item */
                if (!fifo_is_of_length_1(&my_transactions_pending))
                    break;
                pending = fifo_pop_left(&my_transactions_pending);
            }

            lock(&ll_state);
            _add_list(&my_transactions_pending);
        }
    }

    local_list_pending = NULL;
    _Du_AME_FiniThreadDescriptor();
    unlock(&ll_state);
    return NULL;
}

static void _run(void)
{
    pthread_t th[NUMTHREADS];
    int i;
    for (i=0; i<NUMTHREADS; i++) {
        int status = pthread_create(&th[i], NULL, _run_thread, NULL);
        assert(status == 0);
    }
    for (i=0; i<NUMTHREADS; i++) {
        void *result;
        int status = pthread_join(th[i], &result);
        assert(status == 0);
        assert(result == NULL);
    }
}

void Du_TransactionRun(void)
{
    assert(!is_locked(&ll_state));
    assert(!is_locked(&ll_no_tasks_pending));
    if (fifo_is_empty(&global_list_pending))
        return;

    _Du_MakeImmortal();

    num_waiting_threads = 0;
    finished = 0;

    _run();

    assert(finished);
    assert(num_waiting_threads == 0);
    assert(fifo_is_empty(&global_list_pending));
    assert(!is_locked(&ll_state));
    assert(!is_locked(&ll_no_tasks_pending));

    _Du_InitializeObjects();
}
