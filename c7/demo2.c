#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <pthread.h>
#include <semaphore.h>

#include "core.h"


#define LIST_LENGTH 5000
#define BUNCH       400

typedef TLPREFIX struct node_s node_t;
typedef node_t* nodeptr_t;
typedef object_t* objptr_t;

struct node_s {
    struct object_s hdr;
    long value;
    nodeptr_t next;
};


size_t stmcb_size(struct object_s *ob)
{
    return sizeof(struct node_s);
}

void stmcb_trace(struct object_s *obj, void visit(object_t **))
{
    struct node_s *n;
    n = (struct node_s*)obj;
    visit((object_t **)&n->next);
}


nodeptr_t global_chained_list = NULL;


long check_sorted()
{
    nodeptr_t r_n;
    long prev, sum;
    jmpbufptr_t here;

 back:
    if (__builtin_setjmp(here) == 0) {
        stm_start_transaction(&here);
        
        stm_read((objptr_t)global_chained_list);
        r_n = global_chained_list;
        assert(r_n->value == -1);
    
        prev = -1;
        sum = 0;
        while (r_n->next) {
            r_n = r_n->next;
            stm_read((objptr_t)r_n);
            sum += r_n->value;

            _stm_start_safe_point();
            _stm_stop_safe_point();
            if (prev >= r_n->value) {
                stm_stop_transaction();
                return -1;
            }
        
            prev = r_n->value;
        }

        stm_stop_transaction();
        return sum;
    }
    goto back;
}

nodeptr_t swap_nodes(nodeptr_t initial)
{
    jmpbufptr_t here;

    assert(initial != NULL);
 back:
    if (__builtin_setjmp(here) == 0) {
        stm_start_transaction(&here);
        nodeptr_t prev = initial;
        stm_read((objptr_t)prev);
        
        int i;
        for (i=0; i<BUNCH; i++) {
            nodeptr_t current = prev->next;
            if (current == NULL) {
                stm_stop_transaction();
                return NULL;
            }
            stm_read((objptr_t)current);
            nodeptr_t next = current->next;
            if (next == NULL) {
                stm_stop_transaction();
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

                _stm_start_safe_point();
                _stm_stop_safe_point();
            }
            prev = current;
        }

        stm_stop_transaction();
        return prev;
    }
    goto back;
}




void bubble_run()
{
    nodeptr_t r_current;

    r_current = global_chained_list;
    while (r_current) {
        r_current = swap_nodes(r_current);
    }
}


/* initialize list with values in decreasing order */
void setup_list()
{
    int i;
    nodeptr_t w_newnode, w_prev;

    stm_start_transaction(NULL);

    global_chained_list = (nodeptr_t)stm_allocate(sizeof(struct node_s));
    global_chained_list->value = -1;
    global_chained_list->next = NULL;
    
    stm_push_root((objptr_t)global_chained_list);
    
    w_prev = global_chained_list;
    for (i = 0; i < LIST_LENGTH; i++) {
        stm_push_root((objptr_t)w_prev);
        w_newnode = (nodeptr_t)stm_allocate(sizeof(struct node_s));
        
        w_prev = (nodeptr_t)stm_pop_root();
        w_newnode->value = LIST_LENGTH - i;
        w_newnode->next = NULL;

        stm_write((objptr_t)w_prev);
        w_prev->next = w_newnode;
        w_prev = w_newnode;
    }

    stm_stop_transaction();

    global_chained_list = (nodeptr_t)stm_pop_root();
    
    printf("setup ok\n");
}


static sem_t done;
static sem_t go;
static sem_t initialized;


void *demo2(void *arg)
{
    
    int status;
    if (arg != NULL) {
        /* we still need to initialize */
        stm_setup_thread();
        sem_post(&initialized);
        status = sem_wait(&go);
        assert(status == 0);
    }
    
    while (check_sorted() == -1) {
        bubble_run();
    }

    if (arg != NULL) {
        status = sem_post(&done);
        assert(status == 0);
    }
    return NULL;
}

void final_check(void)
{
    long sum;

    printf("final check\n");
    
    sum = check_sorted();
    
    // little Gauss:
    assert(sum == (1 + LIST_LENGTH) * (LIST_LENGTH / 2));
    
    printf("check ok\n");
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

    status = sem_init(&initialized, 0, 0);
    assert(status == 0);
    status = sem_init(&go, 0, 0);
    assert(status == 0);
    
    stm_setup();
    stm_setup_thread();
    
    newthread(demo2, (void*)1);

    status = sem_wait(&initialized);
    assert(status == 0);

    setup_list();

    status = sem_post(&go);
    assert(status == 0);
    
    demo2(NULL);
    
    status = sem_wait(&done);
    assert(status == 0);
        
    final_check();

    return 0;
}
