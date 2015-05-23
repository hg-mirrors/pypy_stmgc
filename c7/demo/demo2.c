#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <pthread.h>
#include <semaphore.h>
#include <time.h>

#ifdef USE_HTM
#  include "../../htm-c7/stmgc.h"
#else
#  include "stmgc.h"
#endif

#define LIST_LENGTH 4000
#define NTHREADS    2

#ifdef USE_HTM
#  define BUNCH       200
#else
#  define BUNCH       200
#endif

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
long stmcb_obj_supports_cards(struct object_s *obj)
{
    return 0;
}
void stmcb_get_card_base_itemsize(struct object_s *obj,
                                  uintptr_t offset_itemsize[2])
{
    abort();
}
void stmcb_trace_cards(struct object_s *obj, void visit(object_t **),
                       uintptr_t start, uintptr_t stop)
{
    abort();
}
void stmcb_commit_soon() {}

static void timing_event(stm_thread_local_t *tl, /* the local thread */
                         enum stm_event_e event,
                         stm_loc_marker_t *markers)
{
    static char *event_names[] = { STM_EVENT_NAMES };

    char buf[1024], *p;
    struct timespec tp;
    clock_gettime(CLOCK_MONOTONIC, &tp);

    p = buf;
    p += sprintf(p, "{%.9f} %p %s", tp.tv_sec + 0.000000001 * tp.tv_nsec,
                 tl, event_names[event]);
    if (markers != NULL) {
        p += sprintf(p, ", markers: %lu, %lu",
                     markers[0].odd_number, markers[1].odd_number);
    }
    sprintf(p, "\n");
    fputs(buf, stderr);
}


nodeptr_t global_chained_list;


long check_sorted(void)
{
    nodeptr_t r_n;
    long prev, sum;

    stm_start_transaction(&stm_thread_local);

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
    assert(initial != NULL);

    stm_start_transaction(&stm_thread_local);

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

    STM_PUSH_ROOT(stm_thread_local, global_chained_list);

    w_prev = global_chained_list;
    for (i = 0; i < LIST_LENGTH; i++) {
        STM_PUSH_ROOT(stm_thread_local, w_prev);
        w_newnode = (nodeptr_t)stm_allocate(sizeof(struct node_s));

        STM_POP_ROOT(stm_thread_local, w_prev);
        w_newnode->value = LIST_LENGTH - i;
        w_newnode->next = NULL;

        stm_write((objptr_t)w_prev);
        w_prev->next = w_newnode;
        w_prev = w_newnode;
    }

    STM_POP_ROOT(stm_thread_local, global_chained_list);   /* update value */
    assert(global_chained_list->value == -1);
    STM_PUSH_ROOT(stm_thread_local, global_chained_list);

    stm_commit_transaction();

    stm_start_transaction(&stm_thread_local);
    STM_POP_ROOT(stm_thread_local, global_chained_list);   /* update value */
    assert(global_chained_list->value == -1);
    STM_PUSH_ROOT(stm_thread_local, global_chained_list);  /* remains forever in the shadow stack */
    stm_commit_transaction();

    printf("setup ok\n");
}

void teardown_list(void)
{
    STM_POP_ROOT_DROP(stm_thread_local);
}


static sem_t done;


void unregister_thread_local(void)
{
    stm_unregister_thread_local(&stm_thread_local);
}

void *demo2(void *arg)
{
    int status;
    rewind_jmp_buf rjbuf;
    stm_register_thread_local(&stm_thread_local);
    stm_rewind_jmp_enterframe(&stm_thread_local, &rjbuf);
    char *org = (char *)stm_thread_local.shadowstack;

    STM_PUSH_ROOT(stm_thread_local, global_chained_list);  /* remains forever in the shadow stack */

    int loops = 0;

    while (check_sorted() == -1) {

        STM_PUSH_MARKER(stm_thread_local, 2 * loops + 1, NULL);

        bubble_run();

        STM_POP_MARKER(stm_thread_local);
        loops++;
    }

    STM_POP_ROOT(stm_thread_local, global_chained_list);
    OPT_ASSERT(org == (char *)stm_thread_local.shadowstack);

    stm_rewind_jmp_leaveframe(&stm_thread_local, &rjbuf);
    unregister_thread_local();
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
    int status, i;
    rewind_jmp_buf rjbuf;

    status = sem_init(&done, 0, 0); assert(status == 0);

    stm_setup();
    stm_register_thread_local(&stm_thread_local);

    /* check that we can use stm_start_inevitable_transaction() without
       any rjbuf on the stack */
    stm_start_inevitable_transaction(&stm_thread_local);
    stm_commit_transaction();


    stm_rewind_jmp_enterframe(&stm_thread_local, &rjbuf);
    stmcb_timing_event = timing_event;

    setup_list();


    for (i = 1; i <= NTHREADS; i++) {
        newthread(demo2, (void*)(uintptr_t)i);
    }

    for (i = 1; i <= NTHREADS; i++) {
        status = sem_wait(&done); assert(status == 0);
    }

    final_check();

    teardown_list();

    stm_rewind_jmp_leaveframe(&stm_thread_local, &rjbuf);
    unregister_thread_local();
    //stm_teardown();

    return 0;
}
