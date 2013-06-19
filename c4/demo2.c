#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <pthread.h>
#include <semaphore.h>

#include "stmgc.h"
#include "fprintcolor.h"


#define LIST_LENGTH 500
#define NUMTHREADS  4


#define GCTID_STRUCT_NODE     123

struct node {
    struct stm_object_s hdr;
    long value;
    struct node *next;
};
typedef struct node * nodeptr;

size_t stmcb_size(gcptr ob)
{
    assert(stm_get_tid(ob) == GCTID_STRUCT_NODE);
    return sizeof(struct node);
}

void stmcb_trace(gcptr ob, void visit(gcptr *))
{
    nodeptr n;
    assert(stm_get_tid(ob) == GCTID_STRUCT_NODE);
    n = (nodeptr)ob;
    visit((gcptr *)&n->next);
}


struct node global_chained_list = {
    { GCTID_STRUCT_NODE | PREBUILT_FLAGS, PREBUILT_REVISION },
    -1,
    NULL,
};


long check_sorted()
{
    nodeptr r_n;
    long prev, sum;
    r_n = (nodeptr)stm_read_barrier((gcptr)&global_chained_list);
    assert(r_n->value == -1);
    
    prev = -1;
    sum = 0;
    while (r_n->next) {
        r_n = (nodeptr)stm_read_barrier((gcptr)r_n->next);
        sum += r_n->value;
        
        if (prev >= r_n->value)
            return -1;
        
        prev = r_n->value;
    }
    
    return sum;
}

void swap_nodes(nodeptr prev, nodeptr current, nodeptr next)
{
    nodeptr w_prev, w_current, w_next;
    w_prev = (nodeptr)stm_write_barrier((gcptr)prev);
    w_current = (nodeptr)stm_write_barrier((gcptr)current);
    w_next = (nodeptr)stm_write_barrier((gcptr)next);
    
    w_prev->next = w_next;
    w_current->next = w_next->next;
    w_next->next = w_current;
}

int bubble_run(gcptr arg1, int retry_counter)
{
    nodeptr r_prev, r_current, r_next, tmp;
    
    r_prev = (nodeptr)stm_read_barrier(arg1);
    r_current = (nodeptr)stm_read_barrier((gcptr)r_prev->next);
    r_next = (nodeptr)stm_read_barrier((gcptr)r_current->next);
    
    while (r_next) {
        if (r_next->value < r_current->value) {
            // swap current and next
            swap_nodes(r_prev, r_current, r_next);	  
            fprintf(stdout, "#");
            
            // needs read barriers, because of write barriers in swap_nodes
            r_prev = (struct node*)stm_read_barrier((gcptr)r_prev);
            tmp = r_current;
            r_current = (struct node*)stm_read_barrier((gcptr)r_next);
            r_next = (struct node*)stm_read_barrier((gcptr)tmp);
        }
        // results from consecutive read_barriers can differ. needs Ptr_Eq()
        /* assert(stm_read_barrier((gcptr)r_prev->next) == r_current */
        /* 	     && stm_read_barrier((gcptr)r_current->next) == r_next); */
        // for now:
        assert(((nodeptr)stm_read_barrier((gcptr)r_prev->next))->value 
               == r_current->value
               && 
               ((nodeptr)stm_read_barrier((gcptr)r_current->next))->value
               == r_next->value);
        
        r_prev = r_current;
        r_current = r_next;
        r_next = r_next->next;
        if (r_next != NULL)
            r_next = (nodeptr)stm_read_barrier((gcptr)r_next);
    }
    
    
    return 0;
}


static sem_t done;

static int thr_mynum = 0;

void *demo2(void *arg)
{
    
    int status;
    stm_initialize();
    
    thr_mynum++;   /* protected by being inevitable here */
    fprintf(stderr, "THREAD STARTING\n");
    
    
    while (check_sorted() == -1) {
        stm_perform_transaction((gcptr)&global_chained_list, bubble_run);
    }
    
    stm_finalize();
    
    status = sem_post(&done);
    assert(status == 0);
    return NULL;
}

void final_check(void)
{
    long sum;
    
    stm_initialize();
    
    sum = check_sorted();
    
    // little Gauss:
    assert(sum == (1 + LIST_LENGTH) * (LIST_LENGTH / 2));
    
    stm_finalize();
    printf("check ok\n");
}


void newthread(void*(*func)(void*), void *arg)
{
    pthread_t th;
    int status = pthread_create(&th, NULL, func, arg);
    assert(status == 0);
    pthread_detach(th);
    printf("started new thread\n");
}


/* initialize list with values in decreasing order */
void setup_list()
{
    int i;
    nodeptr w_newnode, w_prev;
    stm_initialize();
    
    w_prev = &global_chained_list;
    for (i = 0; i < LIST_LENGTH; i++) {
        stm_push_root((gcptr)w_prev);
        w_newnode = (nodeptr)stm_allocate(sizeof(struct node),
                                          GCTID_STRUCT_NODE);
        w_prev = (nodeptr)stm_pop_root();
        w_newnode->value = LIST_LENGTH - i;
        w_newnode->next = NULL;
        
        w_prev = (nodeptr)stm_write_barrier((gcptr)w_prev);
        w_prev->next = w_newnode;
        w_prev = w_newnode;
    }
    
    stm_finalize();
    printf("setup ok\n");
}

int main(void)
{
    int i, status;
    
    setup_list();
    
    status = sem_init(&done, 0, 0);
    assert(status == 0);
    
    for (i = 0; i < NUMTHREADS; i++)
        newthread(demo2, NULL);
    
    for (i=0; i < NUMTHREADS; i++) {
        status = sem_wait(&done);
        assert(status == 0);
        printf("thread finished\n");
    }
    
    final_check();
    return 0;
}
