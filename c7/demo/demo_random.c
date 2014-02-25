#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <pthread.h>
#include <semaphore.h>
#include <string.h>

#include "stmgc.h"

#define NUMTHREADS 2
#define STEPS_PER_THREAD 5000
#define THREAD_STARTS 100 // how many restarts of threads
#define SHARED_ROOTS 3
#define MAXROOTS 1000


// SUPPORT
struct node_s;
typedef TLPREFIX struct node_s node_t;
typedef node_t* nodeptr_t;
typedef object_t* objptr_t;

struct node_s {
    struct object_s hdr;
    long value;
    nodeptr_t next;
};


static sem_t done;
__thread stm_thread_local_t stm_thread_local;

// global and per-thread-data
time_t default_seed;
objptr_t shared_roots[SHARED_ROOTS];

struct thread_data {
    unsigned int thread_seed;
    objptr_t roots[MAXROOTS];
    int num_roots;
    int num_roots_at_transaction_start;
    int steps_left;
};
__thread struct thread_data td;


#define PUSH_ROOT(p)   (*(stm_thread_local.shadowstack++) = (object_t *)(p))
#define POP_ROOT(p)    ((p) = (typeof(p))*(--stm_thread_local.shadowstack))

void init_shadow_stack(void)
{
    object_t **s = (object_t **)malloc(1000 * sizeof(object_t *));
    assert(s);
    stm_thread_local.shadowstack = s;
    stm_thread_local.shadowstack_base = s;
}

void done_shadow_stack(void)
{
    free(stm_thread_local.shadowstack_base);
    stm_thread_local.shadowstack = NULL;
    stm_thread_local.shadowstack_base = NULL;
}


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

void _push_shared_roots()
{
    int i;
    for (i = 0; i < SHARED_ROOTS; i++) {
        PUSH_ROOT(shared_roots[i]);
    }
}

void _pop_shared_roots()
{
    int i;
    for (i = 0; i < SHARED_ROOTS; i++) {
        POP_ROOT(shared_roots[SHARED_ROOTS - i - 1]);
    }
}

int get_rand(int max)
{
    if (max == 0)
        return 0;
    return (int)(rand_r(&td.thread_seed) % (unsigned int)max);
}

objptr_t get_random_root()
{
    int num = get_rand(2);
    if (num == 0 && td.num_roots > 0) {
        num = get_rand(td.num_roots);
        return td.roots[num];
    }
    else {
        num = get_rand(SHARED_ROOTS);
        return shared_roots[num];
    }
}

void reload_roots()
{
    int i;
    assert(td.num_roots == td.num_roots_at_transaction_start);
    for (i = td.num_roots_at_transaction_start - 1; i >= 0; i--) {
        if (td.roots[i])
            POP_ROOT(td.roots[i]);
    }

    for (i = 0; i < td.num_roots_at_transaction_start; i++) {
        if (td.roots[i])
            PUSH_ROOT(td.roots[i]);
    }
}

void push_roots()
{
    int i;
    for (i = td.num_roots_at_transaction_start; i < td.num_roots; i++) {
        if (td.roots[i])
            PUSH_ROOT(td.roots[i]);
    }
}

void pop_roots()
{
    int i;
    for (i = td.num_roots - 1; i >= td.num_roots_at_transaction_start; i--) {
        if (td.roots[i])
            POP_ROOT(td.roots[i]);
    }
}

void del_root(int idx)
{
    int i;
    assert(idx >= td.num_roots_at_transaction_start);

    for (i = idx; i < td.num_roots - 1; i++)
        td.roots[i] = td.roots[i + 1];
    td.num_roots--;
}

void add_root(objptr_t r)
{
    if (r && td.num_roots < MAXROOTS) {
        td.roots[td.num_roots++] = r;
    }
}


void read_barrier(objptr_t p)
{
    if (p != NULL) {
        stm_read(p);
    }
}

void write_barrier(objptr_t p)
{
    if (p != NULL) {
        stm_write(p);
    }
}



objptr_t simple_events(objptr_t p, objptr_t _r)
{
    nodeptr_t w_r;
    int k = get_rand(8);
    int num;

    switch (k) {
    case 0: // remove a root
        if (td.num_roots > td.num_roots_at_transaction_start) {
            num = td.num_roots_at_transaction_start
                + get_rand(td.num_roots - td.num_roots_at_transaction_start);
            del_root(num);
        }
        break;
    case 1: // add 'p' to roots
        add_root(p);
        break;
    case 2: // set 'p' to point to a root
        if (_r)
            p = _r;
        break;
    case 3: // allocate fresh 'p'
        push_roots();
        p = stm_allocate(sizeof(struct node_s));
        pop_roots();
        /* reload_roots not necessary, all are old after start_transaction */
        break;
    case 4:  // read and validate 'p'
        read_barrier(p);
        break;
    case 5: // only do a stm_write_barrier
        write_barrier(p);
        break;
    case 6: // follow p->next
        if (p) {
            read_barrier(p);
            p = (objptr_t)(((nodeptr_t)(p))->next);
        }
        break;
    case 7: // set 'p' as *next in one of the roots
        write_barrier(_r);
        w_r = (nodeptr_t)_r;
        w_r->next = (nodeptr_t)p;
        break;
    }
    return p;
}


objptr_t do_step(objptr_t p)
{
    objptr_t _r;
    int k;

    _r = get_random_root();
    k = get_rand(11);

    if (k < 10)
        p = simple_events(p, _r);
    else if (get_rand(20) == 1) {
        return (objptr_t)-1; // break current
    }
    return p;
}



void setup_thread()
{
    memset(&td, 0, sizeof(struct thread_data));

    /* stupid check because gdb shows garbage
       in td.roots: */
    int i;
    for (i = 0; i < MAXROOTS; i++)
        assert(td.roots[i] == NULL);

    td.thread_seed = default_seed++;
    td.steps_left = STEPS_PER_THREAD;
    td.num_roots = 0;
    td.num_roots_at_transaction_start = 0;
}



void *demo_random(void *arg)
{
    int status;
    stm_register_thread_local(&stm_thread_local);
    init_shadow_stack();

    /* forever on the shadowstack: */
    _push_shared_roots();

    setup_thread();

    objptr_t p = NULL;
    stm_jmpbuf_t here;

    STM_START_TRANSACTION(&stm_thread_local, here);
    assert(td.num_roots >= td.num_roots_at_transaction_start);
    td.num_roots = td.num_roots_at_transaction_start;
    p = NULL;
    pop_roots();                /* does nothing.. */
    reload_roots();

    while (td.steps_left-->0) {
        if (td.steps_left % 8 == 0)
            fprintf(stdout, "#");

        p = do_step(p);

        if (p == (objptr_t)-1) {
            push_roots();
            stm_commit_transaction();

            td.num_roots_at_transaction_start = td.num_roots;

            STM_START_TRANSACTION(&stm_thread_local, here);
            td.num_roots = td.num_roots_at_transaction_start;
            p = NULL;
            pop_roots();
            reload_roots();
        }
    }
    stm_commit_transaction();

    done_shadow_stack();
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


void setup_globals()
{
    int i;

    stm_start_inevitable_transaction(&stm_thread_local);
    for (i = 0; i < SHARED_ROOTS; i++) {
        shared_roots[i] = stm_allocate(sizeof(struct node_s));
        PUSH_ROOT(shared_roots[i]);
    }
    stm_commit_transaction();

    /* make them OLD */

    stm_start_inevitable_transaction(&stm_thread_local);
    /* update now old references: */
    _pop_shared_roots();
    _push_shared_roots();
    stm_commit_transaction();
    /* leave them on this shadow stack forever for major collections */
}

int main(void)
{
    int i, status;

    /* pick a random seed from the time in seconds.
       A bit pointless for now... because the interleaving of the
       threads is really random. */
    default_seed = time(NULL);
    printf("running with seed=%lld\n", (long long)default_seed);

    status = sem_init(&done, 0, 0);
    assert(status == 0);


    stm_setup();
    stm_register_thread_local(&stm_thread_local);
    init_shadow_stack();

    setup_globals();

    int thread_starts = NUMTHREADS * THREAD_STARTS;
    for (i = 0; i < NUMTHREADS; i++) {
        newthread(demo_random, NULL);
        thread_starts--;
    }

    for (i=0; i < NUMTHREADS * THREAD_STARTS; i++) {
        status = sem_wait(&done);
        assert(status == 0);
        printf("thread finished\n");
        if (thread_starts) {
            thread_starts--;
            newthread(demo_random, NULL);
        }
    }

    printf("Test OK!\n");

    _pop_shared_roots();
    done_shadow_stack();
    stm_unregister_thread_local(&stm_thread_local);
    stm_teardown();

    return 0;
}
