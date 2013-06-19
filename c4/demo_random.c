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
#define GCTID_STRUCT_ROOT     123

struct root {
    struct stm_object_s hdr;
    long value;
    struct root *next;
};
typedef struct root * rootptr;

size_t stmcb_size(gcptr ob)
{
    assert(stm_get_tid(ob) == GCTID_STRUCT_ROOT);
    return sizeof(struct root);
}
void stmcb_trace(gcptr ob, void visit(gcptr *))
{
    rootptr n;
    assert(stm_get_tid(ob) == GCTID_STRUCT_ROOT);
    n = (rootptr)ob;
    visit((gcptr *)&n->next);
}

// global and per-thread-data
time_t default_seed;
gcptr shared_roots[SHARED_ROOTS];
__thread unsigned int thread_seed = 0;
__thread gcptr roots[MAXROOTS];
__thread gcptr roots_outside_perform[MAXROOTS];
__thread gcptr current_root = 0;
__thread int num_roots = 0;
__thread int num_roots_outside_perform = 0;
__thread int steps_left;
__thread int interruptible = 0;


// helper functions
int get_rand(int max)
{
    return (int)(rand_r(&thread_seed) % (unsigned int)max);
}

void copy_roots(gcptr *from, gcptr *to)
{
    int i;
    for (i = 0; i < num_roots; i++)
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
    for (i = 0; i < num_roots; i++)
        stm_push_root(roots[i]);
}

void pop_roots()
{
    int i;
    for (i = num_roots - 1; i >= 0; i--)
        roots[i] = stm_pop_root();
}

void del_root(int idx)
{
    int i;
    for (i = idx; i < num_roots - 1; i++)
        roots[i] = roots[i + 1];
}

gcptr allocate_root(size_t size, int tid)
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
    thread_seed = default_seed;
    steps_left = STEPS;
    interruptible = 0;
    
    num_roots = PREBUILT + NUMROOTS;
    for (i = 0; i < PREBUILT; i++) {
        roots[i] = allocate_pseudoprebuilt(sizeof(struct root), 
                                           GCTID_STRUCT_ROOT);
    }
    for (i = PREBUILT; i < PREBUILT + NUMROOTS; i++) {
        roots[i] = allocate_root(sizeof(struct root), GCTID_STRUCT_ROOT);
    }

}

gcptr do_step(gcptr p)
{
    fprintf(stdout, "#");
    
    rootptr w_r, w_sr;
    gcptr _r, _sr;
    int num, k;

    num = get_rand(num_roots);
    _r = roots[num];

    num = get_rand(SHARED_ROOTS);
    _sr = shared_roots[num];

    k = get_rand(14);

    if (!p) // some parts expect it to be != 0
        p = allocate_root(sizeof(struct root), GCTID_STRUCT_ROOT);

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
        if (num_roots < MAXROOTS)
            roots[num_roots++] = p;
        break;
    case 3: // allocate fresh 'p'
        p = allocate_root(sizeof(struct root), GCTID_STRUCT_ROOT);
        break;
    case 4: // set 'p' as *next in one of the roots
        w_r = (rootptr)stm_write_barrier(_r);
        // XXX: do I have to read_barrier(p)?
        w_r->next = (struct root*)p;
        break;
    case 5:  // read and validate 'p'
        stm_read_barrier(p);
        break;
    case 6: // transaction break
        if (interruptible)
            return -1; // break current
        transaction_break();
        p = NULL;
        break;
    case 7: // only do a stm_write_barrier
        p = stm_write_barrier(p);
        break;
    case 8:
        p = (gcptr)(((rootptr)stm_read_barrier(p))->next);
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
        w_sr = stm_write_barrier(_sr);
        w_sr->next = shared_roots[get_rand(SHARED_ROOTS)];
    default:
        break;
    }
    return p;
}


void transaction_break()
{
    push_roots();
    interruptible = 1;
    
    copy_roots(roots, roots_outside_perform);
    num_roots_outside_perform = num_roots;
    
    stm_perform_transaction(NULL, interruptible_callback);
    
    num_roots = num_roots_outside_perform;
    copy_roots(roots_outside_perform, roots);
    
    interruptible = 0;
    pop_roots();
}


int interruptible_callback(gcptr arg1, int retry_counter)
{
    num_roots = num_roots_outside_perform;
    copy_roots(roots_outside_perform, roots);

    arg1 = stm_pop_root();
    pop_roots();
    push_roots();
    stm_push_root(arg1);

    gcptr p = run_me();
    int restart = p == -1 ? get_rand(3) != 1 : 0;

    return restart;
}

int run_me()
{
    gcptr p = NULL;
    while (steps_left) {
        steps_left--;
        p = do_step(p);

        if (p == -1)
            return p;
    }
    return 0;
}


static sem_t done;
static int thr_mynum = 0;

void *demo(void *arg)
{  
    int status, i;
    
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
        shared_roots[i] = allocate_pseudoprebuilt(sizeof(struct root), 
                                                  GCTID_STRUCT_ROOT);
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
