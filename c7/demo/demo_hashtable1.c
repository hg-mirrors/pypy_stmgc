#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <pthread.h>
#include <semaphore.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "stmgc.h"

#define NUMTHREADS  4


typedef TLPREFIX struct node_s node_t;
typedef TLPREFIX struct dict_s dict_t;


struct node_s {
    struct object_s header;
    int typeid;
    intptr_t freevalue;
};

struct dict_s {
    struct node_s hdr;
    stm_hashtable_t *hashtable;
};

#define TID_NODE       0x01234567
#define TID_DICT       0x56789ABC
#define TID_DICTENTRY  0x6789ABCD


static sem_t done;
__thread stm_thread_local_t stm_thread_local;

// global and per-thread-data
time_t default_seed;
dict_t *global_dict;

struct thread_data {
    unsigned int thread_seed;
};
__thread struct thread_data td;


ssize_t stmcb_size_rounded_up(struct object_s *ob)
{
    if (((struct node_s*)ob)->typeid == TID_NODE)
        return sizeof(struct node_s);
    if (((struct node_s*)ob)->typeid == TID_DICT)
        return sizeof(struct dict_s);
    if (((struct node_s*)ob)->typeid == TID_DICTENTRY)
        return sizeof(struct stm_hashtable_entry_s);
    abort();
}

void stmcb_trace(struct object_s *obj, void visit(object_t **))
{
    struct node_s *n;
    n = (struct node_s*)obj;
    if (n->typeid == TID_NODE) {
        return;
    }
    if (n->typeid == TID_DICT) {
        stm_hashtable_tracefn(((struct dict_s *)n)->hashtable, visit);
        return;
    }
    if (n->typeid == TID_DICTENTRY) {
        object_t **ref = &((struct stm_hashtable_entry_s *)obj)->object;
        visit(ref);
        return;
    }
    abort();
}

void stmcb_commit_soon() {}
long stmcb_obj_supports_cards(struct object_s *obj)
{
    return 0;
}
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


void populate_hashtable(int keymin, int keymax)
{
    int i;
    int diff = get_rand(keymax - keymin);
    for (i = 0; i < keymax - keymin; i++) {
        int key = keymin + i + diff;
        if (key >= keymax)
            key -= (keymax - keymin);
        object_t *o = stm_allocate(sizeof(struct node_s));
        ((node_t *)o)->typeid = TID_NODE;
        ((node_t *)o)->freevalue = key;
        assert(global_dict->hdr.freevalue == 42);
        stm_hashtable_write((object_t *)global_dict, global_dict->hashtable,
                            key, o, &stm_thread_local);
    }
}

void setup_thread(void)
{
    memset(&td, 0, sizeof(struct thread_data));
    td.thread_seed = default_seed++;
}

void *demo_random(void *arg)
{
    int threadnum = (uintptr_t)arg;
    int status;
    rewind_jmp_buf rjbuf;
    stm_register_thread_local(&stm_thread_local);
    stm_rewind_jmp_enterframe(&stm_thread_local, &rjbuf);

    setup_thread();

    volatile int start_count = 0;

    stm_start_transaction(&stm_thread_local);
    ++start_count;
    assert(start_count == 1);  // all the writes that follow must not conflict
    populate_hashtable(1291 * threadnum, 1291 * (threadnum + 1));
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

void setup_globals(void)
{
    stm_hashtable_t *my_hashtable = stm_hashtable_create();
    struct dict_s new_templ = {
        .hdr = {
            .typeid = TID_DICT,
            .freevalue = 42,
        },
        .hashtable = my_hashtable,
    };

    stm_start_inevitable_transaction(&stm_thread_local);
    global_dict = (dict_t *)stm_setup_prebuilt(
                      (object_t* )(uintptr_t)&new_templ);
    assert(global_dict->hashtable);
    stm_commit_transaction();
}


int main(void)
{
    int i, status;
    rewind_jmp_buf rjbuf;

    stm_hashtable_entry_userdata = TID_DICTENTRY;

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

    for (i = 0; i < NUMTHREADS; i++) {
        newthread(demo_random, (void *)(uintptr_t)i);
    }

    for (i=0; i < NUMTHREADS; i++) {
        status = sem_wait(&done);
        assert(status == 0);
        printf("thread finished\n");
    }

    printf("Test OK!\n");

    stm_rewind_jmp_leaveframe(&stm_thread_local, &rjbuf);
    stm_unregister_thread_local(&stm_thread_local);
    stm_teardown();

    return 0;
}
