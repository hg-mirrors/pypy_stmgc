#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <pthread.h>
#include <semaphore.h>
#include <string.h>

#include "stmgc.h"

#define NUMTHREADS 3
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
    long my_size;
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


ssize_t stmcb_size_rounded_up(struct object_s *ob)
{
    return ((struct node_s*)ob)->my_size;
}

void stmcb_trace(struct object_s *obj, void visit(object_t **))
{
    struct node_s *n;
    n = (struct node_s*)obj;

    /* and the same value at the end: */
    /* note, ->next may be the same as last_next */
    nodeptr_t *last_next = (nodeptr_t*)((char*)n + n->my_size - sizeof(void*));

    assert(n->next == *last_next);

    visit((object_t **)&n->next);
    visit((object_t **)last_next);

    assert(n->next == *last_next);
}

void _push_shared_roots()
{
    int i;
    for (i = 0; i < SHARED_ROOTS; i++) {
        STM_PUSH_ROOT(stm_thread_local, shared_roots[i]);
    }
}

void _pop_shared_roots()
{
    int i;
    for (i = 0; i < SHARED_ROOTS; i++) {
        STM_POP_ROOT(stm_thread_local, shared_roots[SHARED_ROOTS - i - 1]);
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
            STM_POP_ROOT(stm_thread_local, td.roots[i]);
    }

    for (i = 0; i < td.num_roots_at_transaction_start; i++) {
        if (td.roots[i])
            STM_PUSH_ROOT(stm_thread_local, td.roots[i]);
    }
}

void push_roots()
{
    int i;
    for (i = td.num_roots_at_transaction_start; i < td.num_roots; i++) {
        if (td.roots[i])
            STM_PUSH_ROOT(stm_thread_local, td.roots[i]);
    }
}

void pop_roots()
{
    int i;
    for (i = td.num_roots - 1; i >= td.num_roots_at_transaction_start; i--) {
        if (td.roots[i])
            STM_POP_ROOT(stm_thread_local, td.roots[i]);
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

void set_next(objptr_t p, objptr_t v)
{
    if (p != NULL) {
        nodeptr_t n = (nodeptr_t)p;

        /* and the same value at the end: */
        nodeptr_t TLPREFIX *last_next = (nodeptr_t TLPREFIX *)((stm_char*)n + n->my_size - sizeof(void*));
        assert(n->next == *last_next);
        n->next = (nodeptr_t)v;
        *last_next = (nodeptr_t)v;
    }
}

nodeptr_t get_next(objptr_t p)
{
    nodeptr_t n = (nodeptr_t)p;

    /* and the same value at the end: */
    nodeptr_t TLPREFIX *last_next = (nodeptr_t TLPREFIX *)((stm_char*)n + n->my_size - sizeof(void*));
    assert(n->next == *last_next);

    return n->next;
}


objptr_t simple_events(objptr_t p, objptr_t _r)
{
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
        size_t sizes[4] = {sizeof(struct node_s),
                           sizeof(struct node_s) + 48,
                           sizeof(struct node_s) + 4096,
                           sizeof(struct node_s) + 4096*70};
        size_t size = sizes[get_rand(4)];
        p = stm_allocate(size);
        ((nodeptr_t)p)->my_size = size;
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
            p = (objptr_t)(get_next(p));
        }
        break;
    case 7: // set 'p' as *next in one of the roots
        write_barrier(_r);
        set_next(_r, p);
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
        ((nodeptr_t)shared_roots[i])->my_size = sizeof(struct node_s);
        STM_PUSH_ROOT(stm_thread_local, shared_roots[i]);
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
    stm_unregister_thread_local(&stm_thread_local);
    stm_teardown();

    return 0;
}