#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "core.h"


#define NUM_THREADS 4


typedef struct {
    struct object_s header;
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

void do_test(void)
{
    int i;
    pid_t child_pids[NUM_THREADS];

    for (i = 0; i < NUM_THREADS; i++) {
        child_pids[i] = fork();
        if (child_pids[i] == -1) {
            perror("fork");
            abort();
        }
        if (child_pids[i] == 0) {
            stm_setup_process();
            do_run_in_thread(i);
            exit(0);
        }
    }

    for (i = 0; i < NUM_THREADS; i++) {
        int status;
        if (waitpid(child_pids[i], &status, 0) == -1) {
            perror("waitpid");
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
