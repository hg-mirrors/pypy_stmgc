#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <pthread.h>
#include <errno.h>

#include "core.h"


#define NUM_THREADS 4


typedef struct {
    object_t header;
    int val1, val2;
} obj_t;

void do_run_in_thread(int i)
{
    stm_start_transaction();
    obj_t *ob1 = (obj_t *)stm_allocate(16);
    obj_t *ob2 = (obj_t *)stm_allocate(16);

    assert(!_stm_was_read(&ob1->header));
    assert(!_stm_was_read(&ob2->header));
    stm_read(&ob1->header);
    stm_read(&ob2->header);
    assert(_stm_was_read(&ob1->header));
    assert(_stm_was_read(&ob2->header));
    assert(_stm_was_written(&ob1->header));
    assert(_stm_was_written(&ob2->header));
    stm_write(&ob1->header);
    stm_write(&ob2->header);
    assert(_stm_was_written(&ob1->header));
    assert(_stm_was_written(&ob2->header));
    ob1->val1 = 100;
    ob1->val2 = 200;
    ob2->val1 = 300;
    ob2->val2 = 400;

    stm_stop_transaction();

    int j;
    for (j=0; j<2; j++) {
        stm_start_transaction();

        assert(!_stm_was_read(&ob1->header));
        assert(!_stm_was_read(&ob2->header));
        assert(!_stm_was_written(&ob1->header));
        assert(!_stm_was_written(&ob2->header));
        stm_read(&ob1->header);
        printf("thread %d: ob1.val2=%d\n", i, ob1->val2);
        
        stm_write(&ob1->header);
        assert(_stm_was_written(&ob1->header));
        assert(!_stm_was_written(&ob2->header));
        
        stm_stop_transaction();
    }

    printf("thread %d: %p, %p\n", i, ob1, ob2);
}

static void *run_in_thread(void *arg)
{
    stm_setup_thread();
    do_run_in_thread((intptr_t)arg);
    return NULL;
}

void do_test(void)
{
    int i, res;
    pthread_t threads[NUM_THREADS];

    for (i = 0; i < NUM_THREADS; i++) {
        res = pthread_create(&threads[i], NULL, run_in_thread,
                             (void *)(intptr_t)i);
        if (res != 0) {
            errno = res;
            perror("pthread_create");
            abort();
        }
    }

    for (i = 0; i < NUM_THREADS; i++) {
        res = pthread_join(threads[i], NULL);
        if (res != 0) {
            errno = res;
            perror("pthread_join");
            abort();
        }
    }
}


int main(int argc, char *argv[])
{
    stm_setup();

    do_test();
    
    return 0;
}
