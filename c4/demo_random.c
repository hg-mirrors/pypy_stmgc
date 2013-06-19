#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <pthread.h>
#include <semaphore.h>
#include <time.h>

#include "stmgc.h"
#include "fprintcolor.h"


#define NUMTHREADS 4
#define STEPS 100000
#define NUMROOTS 10
#define PREBUILT 3
#define MAXROOTS 1000
#define SHARED_ROOTS 5


// SUPPORT
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


// global and per-thread-data
time_t default_seed;
gcptr shared_roots[SHARED_ROOTS];
struct thread_data {
    unsigned int thread_seed;
    gcptr roots[MAXROOTS];
    gcptr roots_outside_perform[MAXROOTS];
    gcptr current_root;
    int num_roots;
    int num_roots_outside_perform;
    int steps_left;
    int interruptible;
};
__thread struct thread_data td;


// helper functions
int get_rand(int max)
{
    return (int)(rand_r(&td.thread_seed) % (unsigned int)max);
}

void copy_roots(gcptr *from, gcptr *to, int num)
{
    int i;
    for (i = 0; i < num; i++)
        *(to++) = *(from++);
}

gcptr allocate_pseudoprebuilt(size_t size, int tid)
{
    gcptr x = calloc(1, size);
    x->h_tid = PREBUILT_FLAGS | tid;
    x->h_revision = PREBUILT_REVISION;
    return x;
}

void push_roots()
{
    int i;
    for (i = 0; i < td.num_roots; i++)
        stm_push_root(td.roots[i]);
}

void pop_roots()
{
    int i;
    for (i = td.num_roots - 1; i >= 0; i--)
        td.roots[i] = stm_pop_root();
}

void del_root(int idx)
{
    int i;
    for (i = idx; i < td.num_roots - 1; i++)
        td.roots[i] = td.roots[i + 1];
}

gcptr allocate_node(size_t size, int tid)
{
    gcptr r;
    push_roots();
    r = stm_allocate(size, tid);
    pop_roots();
    return r;
}



// THREAD TESTER
int interruptible_callback(gcptr arg1, int retry_counter);
int run_me();
void transaction_break();

void setup_thread()
{
    int i;
    td.thread_seed = default_seed;
    td.steps_left = STEPS;
    td.interruptible = 0;
    
    td.num_roots = PREBUILT + NUMROOTS;
    for (i = 0; i < PREBUILT; i++) {
        td.roots[i] = allocate_pseudoprebuilt(sizeof(struct node), 
                                              GCTID_STRUCT_NODE);
    }
    for (i = PREBUILT; i < PREBUILT + NUMROOTS; i++) {
        td.roots[i] = allocate_node(sizeof(struct node), GCTID_STRUCT_NODE);
    }

}

gcptr do_step(gcptr p)
{
    fprintf(stdout, "#");
    
    nodeptr w_r, w_sr;
    gcptr _r, _sr;
    int num, k;

    num = get_rand(td.num_roots);
    _r = td.roots[num];

    num = get_rand(SHARED_ROOTS);
    _sr = shared_roots[num];

    k = get_rand(14);

    if (!p) // some parts expect it to be != 0
        p = allocate_node(sizeof(struct node), GCTID_STRUCT_NODE);

    switch (k) {
    case 0: // remove a root
        if (num > 0)
            del_root(num);
        break;
    case 1: // set 'p' to point to a root
        if (_r)
            p = _r;
        break;
    case 2: // add 'p' to roots
        if (td.num_roots < MAXROOTS)
            td.roots[td.num_roots++] = p;
        break;
    case 3: // allocate fresh 'p'
        p = allocate_node(sizeof(struct node), GCTID_STRUCT_NODE);
        break;
    case 4: // set 'p' as *next in one of the roots
        w_r = (nodeptr)stm_write_barrier(_r);
        // XXX: do I have to read_barrier(p)?
        w_r->next = (struct node*)p;
        break;
    case 5:  // read and validate 'p'
        stm_read_barrier(p);
        break;
    case 6: // transaction break
        if (td.interruptible)
            return (gcptr)-1; // break current
        transaction_break();
        p = NULL;
        break;
    case 7: // only do a stm_write_barrier
        p = stm_write_barrier(p);
        break;
    case 8:
        p = (gcptr)(((nodeptr)stm_read_barrier(p))->next);
        break;
    case 9: // XXX: rare events
        break;
    case 10: // only do a stm_read_barrier
        p = stm_read_barrier(p);
        break;
    case 11:
        stm_read_barrier(_sr);
        break;
    case 12:
        stm_write_barrier(_sr);
        break;
    case 13:
        w_sr = (nodeptr)stm_write_barrier(_sr);
        w_sr->next = (nodeptr)shared_roots[get_rand(SHARED_ROOTS)];
    default:
        break;
    }
    return p;
}


void transaction_break()
{
    push_roots();
    td.interruptible = 1;
    
    copy_roots(td.roots, td.roots_outside_perform, td.num_roots);
    td.num_roots_outside_perform = td.num_roots;
    
    stm_perform_transaction(NULL, interruptible_callback);
    
    td.num_roots = td.num_roots_outside_perform;
    copy_roots(td.roots_outside_perform, td.roots, td.num_roots);
    
    td.interruptible = 0;
    pop_roots();
}


int interruptible_callback(gcptr arg1, int retry_counter)
{
    td.num_roots = td.num_roots_outside_perform;
    copy_roots(td.roots_outside_perform, td.roots, td.num_roots);

    arg1 = stm_pop_root();
    pop_roots();
    push_roots();
    stm_push_root(arg1);

    int p = run_me();
    int restart = p == -1 ? get_rand(3) != 1 : 0;

    return restart;
}

int run_me()
{
    gcptr p = NULL;
    while (td.steps_left) {
        td.steps_left--;
        p = do_step(p);

        if (p == (gcptr)-1)
            return -1;
    }
    return 0;
}


static sem_t done;
static int thr_mynum = 0;

void *demo(void *arg)
{  
    int status;
    
    stm_initialize();
    setup_thread();
    
    thr_mynum++;   /* protected by being inevitable here */
    fprintf(stderr, "THREAD STARTING\n");

    run_me();
    
    stm_finalize();
    
    status = sem_post(&done);
    assert(status == 0);
    return NULL;
}


void newthread(void*(*func)(void*), void *arg)
{
    pthread_t th;
    int status = pthread_create(&th, NULL, func, arg);
    assert(status == 0);
    pthread_detach(th);
    printf("started new thread\n");
}



int main(void)
{
    int i, status;
    
    // seed changes daily
    // a bit pointless for now..
    default_seed = time(NULL) / 3600 / 24;
    
    for (i = 0; i < SHARED_ROOTS; i++) {
        shared_roots[i] = allocate_pseudoprebuilt(sizeof(struct node), 
                                                  GCTID_STRUCT_NODE);
    }    
    
    status = sem_init(&done, 0, 0);
    assert(status == 0);
    
    for (i = 0; i < NUMTHREADS; i++)
        newthread(demo, NULL);
    
    for (i=0; i < NUMTHREADS; i++) {
        status = sem_wait(&done);
        assert(status == 0);
        printf("thread finished\n");
    }
    
    return 0;
}
