#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <pthread.h>
#include <semaphore.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "stmgc.h"

#define NUMTHREADS 3
#define STEPS_PER_THREAD 500
#define THREAD_STARTS 1000 // how many restarts of threads
#define PREBUILT_ROOTS 3
#define FORKS 3

#define ACTIVE_ROOTS_SET_SIZE 100 // max num of roots created/alive in one transaction
#define MAX_ROOTS_ON_SS 1000 // max on shadow stack

// SUPPORT
struct node_s;
typedef TLPREFIX struct node_s node_t;
typedef node_t* nodeptr_t;
typedef object_t* objptr_t;
int num_forked_children = 0;

struct node_s {
    struct object_s hdr;
    int sig;
    long my_size;
    long my_id;
    long my_hash;
    nodeptr_t next;
};

#define SIGNATURE 0x01234567


static sem_t done;
__thread stm_thread_local_t stm_thread_local;
__thread void *thread_may_fork;

// global and per-thread-data
time_t default_seed;
objptr_t prebuilt_roots[PREBUILT_ROOTS];

struct thread_data {
    unsigned int thread_seed;
    int steps_left;
    objptr_t active_roots_set[ACTIVE_ROOTS_SET_SIZE];
    int active_roots_num;
    long roots_on_ss;
    long roots_on_ss_at_tr_start;
};
__thread struct thread_data td;

struct thread_data *_get_td(void)
{
    return &td;     /* for gdb */
}


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

void stmcb_commit_soon() {}

void stmcb_trace_cards(struct object_s *obj, void cb(object_t **),
                       uintptr_t start, uintptr_t stop) {
    abort();
}
void stmcb_get_card_base_itemsize(struct object_s *obj,
                                  uintptr_t offset_itemsize[2]) {
    abort();
}

int get_rand(int max)
{
    if (max == 0)
        return 0;
    return (int)(rand_r(&td.thread_seed) % (unsigned int)max);
}

objptr_t get_random_root()
{
    /* get some root from shadowstack or active_root_set or prebuilt_roots */
    int num = get_rand(3);
    intptr_t ss_size = td.roots_on_ss;
    if (num == 0 && ss_size > 0) {
        num = get_rand(ss_size);
        /* XXX: impl detail: there is already a "-1" on the SS -> +1 */
        objptr_t r = (objptr_t)stm_thread_local.shadowstack_base[num+1].ss;
        assert((((uintptr_t)r) & 3) == 0);
    }

    if (num == 1 && td.active_roots_num > 0) {
        num = get_rand(td.active_roots_num);
        return td.active_roots_set[num];
    } else {
        num = get_rand(PREBUILT_ROOTS);
        return prebuilt_roots[num];
    }
}


long push_roots()
{
    int i;
    long to_push = td.active_roots_num;
    long not_pushed = 0;
    for (i = to_push - 1; i >= 0; i--) {
        td.active_roots_num--;
        if (td.roots_on_ss < MAX_ROOTS_ON_SS) {
            STM_PUSH_ROOT(stm_thread_local, td.active_roots_set[i]);
            td.roots_on_ss++;
        } else {
            not_pushed++;
        }
    }
    return to_push - not_pushed;
}

void add_root(objptr_t r);
void pop_roots(long to_pop)
{
    int i;
    for (i = 0; i < to_pop; i++) {
        objptr_t t;
        STM_POP_ROOT(stm_thread_local, t);
        add_root(t);
        td.roots_on_ss--;
    }
}

void del_root(int idx)
{
    int i;

    for (i = idx; i < td.active_roots_num - 1; i++)
        td.active_roots_set[i] = td.active_roots_set[i + 1];
    td.active_roots_num--;
}

void add_root(objptr_t r)
{
    if (r && td.active_roots_num < ACTIVE_ROOTS_SET_SIZE) {
        td.active_roots_set[td.active_roots_num++] = r;
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
    OPT_ASSERT(n->next == *last_next);

    return n->next;
}


objptr_t simple_events(objptr_t p, objptr_t _r)
{
    int k = get_rand(10);
    long pushed;

    switch (k) {
    case 0: // remove a root
        if (td.active_roots_num) {
            del_root(get_rand(td.active_roots_num));
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
        pushed = push_roots();
        size_t sizes[4] = {sizeof(struct node_s),
                           sizeof(struct node_s) + (get_rand(100000) & ~15),
                           sizeof(struct node_s) + 4096,
                           sizeof(struct node_s) + 4096*70};
        size_t size = sizes[get_rand(4)];
        p = stm_allocate(size);
        ((nodeptr_t)p)->sig = SIGNATURE;
        ((nodeptr_t)p)->my_size = size;
        ((nodeptr_t)p)->my_id = 0;
        ((nodeptr_t)p)->my_hash = 0;
        pop_roots(pushed);
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
    case 8: // id checking
        if (p) {
            nodeptr_t n = (nodeptr_t)p;
            if (n->my_id == 0) {
                write_barrier(p);
                n->my_id = stm_id(p);
            }
            else {
                read_barrier(p);
                assert(n->my_id == stm_id(p));
            }
        }
        break;
    case 9:
        if (p) {
            nodeptr_t n = (nodeptr_t)p;
            if (n->my_hash == 0) {
                write_barrier(p);
                n->my_hash = stm_identityhash(p);
            }
            else {
                read_barrier(p);
                assert(n->my_hash == stm_identityhash(p));
            }
        }
        break;
    }
    return p;
}

void frame_loop();
objptr_t do_step(objptr_t p)
{
    objptr_t _r;
    int k;

    _r = get_random_root();
    k = get_rand(12);

    if (k < 10) {
        p = simple_events(p, _r);
    } else if (get_rand(20) == 1) {
        long pushed = push_roots();
        stm_commit_transaction();
        td.roots_on_ss_at_tr_start = td.roots_on_ss;

        if (get_rand(100) < 98) {
            stm_start_transaction(&stm_thread_local);
        } else {
            stm_start_inevitable_transaction(&stm_thread_local);
        }
        td.roots_on_ss = td.roots_on_ss_at_tr_start;
        td.active_roots_num = 0;
        pop_roots(pushed);
        p = NULL;
    } else if (get_rand(10) == 1) {
        long pushed = push_roots();
        /* leaving our frame */
        frame_loop();
        /* back in our frame */
        pop_roots(pushed);
        p = NULL;
    } else if (get_rand(20) == 1) {
        long pushed = push_roots();
        stm_become_inevitable(&stm_thread_local, "please");
        assert(stm_is_inevitable());
        pop_roots(pushed);
        p= NULL;
    } else if (get_rand(20) == 1) {
        p = (objptr_t)-1; // possibly fork
    } else if (get_rand(20) == 1) {
        long pushed = push_roots();
        stm_become_globally_unique_transaction(&stm_thread_local, "really");
        fprintf(stderr, "[GUT/%d]", (int)STM_SEGMENT->segment_num);
        pop_roots(pushed);
        p = NULL;
    }
    return p;
}

void frame_loop()
{
    objptr_t p = NULL;
    rewind_jmp_buf rjbuf;

    stm_rewind_jmp_enterframe(&stm_thread_local, &rjbuf);
    //fprintf(stderr,"%p F: %p\n", STM_SEGMENT->running_thread,  __builtin_frame_address(0));

    long roots_on_ss = td.roots_on_ss;
    /* "interpreter main loop": this is one "application-frame" */
    while (td.steps_left-->0 && get_rand(10) != 0) {
        if (td.steps_left % 8 == 0)
            fprintf(stdout, "#");

        assert(p == NULL || ((nodeptr_t)p)->sig == SIGNATURE);

        p = do_step(p);


        if (p == (objptr_t)-1) {
            p = NULL;

            long call_fork = (thread_may_fork != NULL && *(long *)thread_may_fork);
            if (call_fork) {   /* common case */
                long pushed = push_roots();
                /* run a fork() inside the transaction */
                printf("==========   FORK  =========\n");
                *(long*)thread_may_fork = 0;
                pid_t child = fork();
                printf("=== in process %d thread %lx, fork() returned %d\n",
                       (int)getpid(), (long)pthread_self(), (int)child);
                if (child == -1) {
                    fprintf(stderr, "fork() error: %m\n");
                    abort();
                }
                if (child != 0)
                    num_forked_children++;
                else
                    num_forked_children = 0;

                pop_roots(pushed);
            }
        }
    }
    assert(roots_on_ss == td.roots_on_ss);

    stm_rewind_jmp_leaveframe(&stm_thread_local, &rjbuf);
}



void setup_thread()
{
    memset(&td, 0, sizeof(struct thread_data));

    /* stupid check because gdb shows garbage
       in td.roots: */
    int i;
    for (i = 0; i < ACTIVE_ROOTS_SET_SIZE; i++)
        assert(td.active_roots_set[i] == NULL);

    td.thread_seed = default_seed++;
    td.steps_left = STEPS_PER_THREAD;
    td.active_roots_num = 0;
    td.roots_on_ss = 0;
    td.roots_on_ss_at_tr_start = 0;
}



void *demo_random(void *arg)
{
    int status;
    rewind_jmp_buf rjbuf;
    stm_register_thread_local(&stm_thread_local);
    stm_rewind_jmp_enterframe(&stm_thread_local, &rjbuf);

    setup_thread();

    td.roots_on_ss_at_tr_start = 0;
    stm_start_transaction(&stm_thread_local);
    td.roots_on_ss = td.roots_on_ss_at_tr_start;
    td.active_roots_num = 0;

    thread_may_fork = arg;
    while (td.steps_left-->0) {
        frame_loop();
    }

    stm_commit_transaction();

    stm_rewind_jmp_leaveframe(&stm_thread_local, &rjbuf);
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

    struct node_s prebuilt_template = {
        .sig = SIGNATURE,
        .my_size = sizeof(struct node_s),
        .my_id = 0,
        .my_hash = 0,
        .next = NULL
    };

    stm_start_inevitable_transaction(&stm_thread_local);
    for (i = 0; i < PREBUILT_ROOTS; i++) {
        void* new_templ = malloc(sizeof(struct node_s));
        memcpy(new_templ, &prebuilt_template, sizeof(struct node_s));
        prebuilt_roots[i] = stm_setup_prebuilt((objptr_t)(long)new_templ);

        if (i % 2 == 0) {
            int hash = i + 5;
            stm_set_prebuilt_identityhash(prebuilt_roots[i],
                                          hash);
            ((nodeptr_t)prebuilt_roots[i])->my_hash = hash;
        }
    }
    stm_commit_transaction();
}

int main(void)
{
    int i, status;
    rewind_jmp_buf rjbuf;

    /* pick a random seed from the time in seconds.
       A bit pointless for now... because the interleaving of the
       threads is really random. */
    default_seed = time(NULL);
    printf("running with seed=%lld\n", (long long)default_seed);

    status = sem_init(&done, 0, 0);
    assert(status == 0);


    stm_setup();
    stm_register_thread_local(&stm_thread_local);
    stm_rewind_jmp_enterframe(&stm_thread_local, &rjbuf);

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
            long forkbase = NUMTHREADS * THREAD_STARTS / (FORKS + 1);
            long _fork = (thread_starts % forkbase) == 0;
            thread_starts--;
            newthread(demo_random, &_fork);
        }
    }

    for (i = 0; i < num_forked_children; i++) {
        pid_t child = wait(&status);
        if (child == -1)
            perror("wait");
        printf("From %d: child %d terminated with exit status %d\n",
               (int)getpid(), (int)child, status);
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
            ;
        else {
            printf("*** error from the child ***\n");
            return 1;
        }
    }

    printf("Test OK!\n");

    stm_rewind_jmp_leaveframe(&stm_thread_local, &rjbuf);
    stm_unregister_thread_local(&stm_thread_local);
    stm_teardown();

    return 0;
}
