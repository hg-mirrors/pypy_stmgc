#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <pthread.h>
#include <semaphore.h>

#ifdef USE_HTM
#  include "../../htm-c7/stmgc.h"
#else
#  include "stmgc.h"
#endif

#define ITERS 1000000
#define NTHREADS    2


typedef TLPREFIX struct node_s node_t;
typedef node_t* nodeptr_t;
typedef object_t* objptr_t;

struct node_s {
    struct object_s hdr;
    long value;
    nodeptr_t next;
};

__thread stm_thread_local_t stm_thread_local;


ssize_t stmcb_size_rounded_up(struct object_s *ob)
{
    return sizeof(struct node_s);
}

void stmcb_trace(struct object_s *obj, void visit(object_t **))
{
    struct node_s *n;
    n = (struct node_s*)obj;
    visit((object_t **)&n->next);
}

void stmcb_commit_soon() {}

void stmcb_trace_cards(struct object_s *obj, void cb(object_t **),
                       uintptr_t start, uintptr_t stop) {
    abort();
}
void stmcb_get_card_base_itemsize(struct object_s *obj,
                                  uintptr_t offset_itemsize[2]) {
    abort();
}


static sem_t done;

static __thread int tl_counter = 0;
//static int gl_counter = 0;

void *demo2(void *arg)
{
    int status;
    stm_register_thread_local(&stm_thread_local);
    char *org = (char *)stm_thread_local.shadowstack;
    tl_counter = 0;

    object_t *tmp;
    int i = 0;
    while (i < ITERS) {
        stm_start_inevitable_transaction(&stm_thread_local);
        tl_counter++;
        if (i % 500 < 250)
            STM_PUSH_ROOT(stm_thread_local, stm_allocate(16));//gl_counter++;
        else
            STM_POP_ROOT(stm_thread_local, tmp);
        stm_commit_transaction();
        i++;
    }

    assert(org == (char *)stm_thread_local.shadowstack);

    stm_unregister_thread_local(&stm_thread_local);
    status = sem_post(&done); assert(status == 0);
    return NULL;
}


void newthread(void*(*func)(void*), void *arg)
{
    pthread_t th;
    int status = pthread_create(&th, NULL, func, arg);
    if (status != 0)
        abort();
    pthread_detach(th);
    printf("started new thread\n");
}



int main(void)
{
    int status, i;

    status = sem_init(&done, 0, 0); assert(status == 0);

    stm_setup();
    stm_register_thread_local(&stm_thread_local);


    for (i = 1; i <= NTHREADS; i++) {
        newthread(demo2, (void*)(uintptr_t)i);
    }

    for (i = 1; i <= NTHREADS; i++) {
        status = sem_wait(&done); assert(status == 0);
    }


    stm_unregister_thread_local(&stm_thread_local);
    stm_teardown();

    return 0;
}
