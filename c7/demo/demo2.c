#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <pthread.h>
#include <semaphore.h>

#include "stmgc.h"

#define LIST_LENGTH 6000
#define BUNCH       400

typedef TLPREFIX struct node_s node_t;
typedef node_t* nodeptr_t;
typedef object_t* objptr_t;

struct node_s {
    struct object_s hdr;
    long value;
    nodeptr_t next;
};

__thread stm_thread_local_t stm_thread_local;

#define PUSH_ROOT(p)   (void)0 // XXX...
#define POP_ROOT(p)    (void)0 // XXX...


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


nodeptr_t global_chained_list;


long check_sorted(void)
{
    nodeptr_t r_n;
    long prev, sum;
    stm_jmpbuf_t here;

    STM_START_TRANSACTION(&stm_thread_local, here);

    stm_read((objptr_t)global_chained_list);
    r_n = global_chained_list;
    assert(r_n->value == -1);

    prev = -1;
    sum = 0;
    while (r_n->next) {
        r_n = r_n->next;
        stm_read((objptr_t)r_n);
        sum += r_n->value;

        stm_safe_point();
        if (prev >= r_n->value) {
            stm_commit_transaction();
            return -1;
        }

        prev = r_n->value;
    }

    stm_commit_transaction();
    return sum;
}

nodeptr_t swap_nodes(nodeptr_t initial)
{
    stm_jmpbuf_t here;

    assert(initial != NULL);

    STM_START_TRANSACTION(&stm_thread_local, here);

    nodeptr_t prev = initial;
    stm_read((objptr_t)prev);

    int i;
    for (i=0; i<BUNCH; i++) {
        nodeptr_t current = prev->next;
        if (current == NULL) {
            stm_commit_transaction();
            return NULL;
        }
        stm_read((objptr_t)current);
        nodeptr_t next = current->next;
        if (next == NULL) {
            stm_commit_transaction();
            return NULL;
        }
        stm_read((objptr_t)next);

        if (next->value < current->value) {
            stm_write((objptr_t)prev);
            stm_write((objptr_t)current);
            stm_write((objptr_t)next);

            prev->next = next;
            current->next = next->next;
            next->next = current;

            stm_safe_point();
        }
        prev = current;
    }

    stm_commit_transaction();
    return prev;
}



void bubble_run(void)
{
    nodeptr_t r_current;

    r_current = global_chained_list;
    while (r_current) {
        r_current = swap_nodes(r_current);
    }
}


/* initialize list with values in decreasing order */
void setup_list(void)
{
    int i;
    nodeptr_t w_newnode, w_prev;

    stm_start_inevitable_transaction(&stm_thread_local);

    global_chained_list = (nodeptr_t)stm_allocate(sizeof(struct node_s));
    global_chained_list->value = -1;
    global_chained_list->next = NULL;

    PUSH_ROOT(global_chained_list);

    w_prev = global_chained_list;
    for (i = 0; i < LIST_LENGTH; i++) {
        PUSH_ROOT(w_prev);
        w_newnode = (nodeptr_t)stm_allocate(sizeof(struct node_s));

        POP_ROOT(w_prev);
        w_newnode->value = LIST_LENGTH - i;
        w_newnode->next = NULL;

        stm_write((objptr_t)w_prev);
        w_prev->next = w_newnode;
        w_prev = w_newnode;
    }

    POP_ROOT(global_chained_list);   /* update value */
    assert(global_chained_list->value == -1);
    PUSH_ROOT(global_chained_list);

    stm_commit_transaction();

    stm_start_inevitable_transaction(&stm_thread_local);
    POP_ROOT(global_chained_list);   /* update value */
    assert(global_chained_list->value == -1);
    PUSH_ROOT(global_chained_list);  /* remains forever in the shadow stack */
    stm_commit_transaction();

    printf("setup ok\n");
}


static sem_t done;


void *demo2(void *arg)
{
    int status;
    stm_register_thread_local(&stm_thread_local);
    PUSH_ROOT(global_chained_list);  /* remains forever in the shadow stack */

    while (check_sorted() == -1) {
        bubble_run();
    }

    POP_ROOT(global_chained_list);
    assert(stm_thread_local.shadowstack == stm_thread_local.shadowstack_base);
    stm_unregister_thread_local(&stm_thread_local);
    status = sem_post(&done); assert(status == 0);
    return NULL;
}

void final_check(void)
{
    long sum;

    printf("final check\n");
    
    sum = check_sorted();
    
    // little Gauss:
    if (sum == (1 + LIST_LENGTH) * (LIST_LENGTH / 2))
        printf("check ok\n");
    else
        printf("check ERROR\n");
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
    int status;

    status = sem_init(&done, 0, 0); assert(status == 0);

    stm_setup();
    stm_register_thread_local(&stm_thread_local);

    setup_list();

    newthread(demo2, (void*)1);
    newthread(demo2, (void*)2);

    status = sem_wait(&done); assert(status == 0);
    status = sem_wait(&done); assert(status == 0);

    final_check();

    stm_unregister_thread_local(&stm_thread_local);
    stm_teardown();

    return 0;
}
