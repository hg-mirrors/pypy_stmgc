/* -*- c-basic-offset: 2 -*- */

#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <pthread.h>
#include <semaphore.h>


#include "stmgc.h"


#define UPPER_BOUND 10
#define NUMTHREADS  4


#define GCTID_STRUCT_NODE     123

struct node {
    struct stm_object_s hdr;
    long value;
    struct node *next;
};

size_t stmcb_size(gcptr ob)
{
    assert(stm_get_tid(ob) == GCTID_STRUCT_NODE);
    return sizeof(struct node);
}

void stmcb_trace(gcptr ob, void visit(gcptr *))
{
    struct node *n;
    assert(stm_get_tid(ob) == GCTID_STRUCT_NODE);
    n = (struct node *)ob;
    visit((gcptr *)&n->next);
}

struct node global_chained_list = {
    { GCTID_STRUCT_NODE | PREBUILT_FLAGS, PREBUILT_REVISION },
    -1,
    NULL,
};

struct node *do_a_check(int seen[], int stress_gc)
{
  int i;
  for (i=0; i<UPPER_BOUND; i++)
    seen[i] = 0;

  struct node *r_n, *prev_n;
  r_n = (struct node *)stm_read_barrier((gcptr)&global_chained_list);
  assert(r_n->value == -1);
  while (r_n->next)
    {
      prev_n = r_n;   /* for gdb only */

      if (stress_gc)
        {
          /* allocate a young object that is forgotten, to stress the GC */
          stm_push_root((gcptr)r_n);
          stm_allocate(sizeof(struct node), GCTID_STRUCT_NODE);
          r_n = (struct node *)stm_pop_root();
        }

      r_n = (struct node *)stm_read_barrier((gcptr)r_n->next);
      long v = r_n->value;
      assert(0 <= v && v < UPPER_BOUND);
      if (v == 0)
        assert(seen[v] < NUMTHREADS);
      else
        assert(seen[v] < seen[v-1]);
      seen[v]++;
    }
  return r_n;
}

int insert1(gcptr arg1, int retry_counter)
{
    int seen[UPPER_BOUND];
    long nvalue;
    struct node *r_arg;

    r_arg = (struct node *)stm_read_barrier(arg1);
    nvalue = r_arg->value;

    struct node *last = do_a_check(seen, 1);


    stm_push_root((gcptr)last);
    struct node *w_newnode = (struct node *)stm_allocate(sizeof(struct node),
                                                         GCTID_STRUCT_NODE);
    last = (struct node *)stm_pop_root();

    w_newnode->value = nvalue;
    w_newnode->next = NULL;

    struct node *w_last = (struct node *)stm_write_barrier((gcptr)last);
    w_last->next = w_newnode;

    return 0;   /* return from stm_perform_transaction() */
}

static sem_t done;

extern void stmgcpage_possibly_major_collect(int force);  /* temp */

void *demo1(void *arg)
{
    int i, status;
    struct node *w_node;
    stm_initialize();

    w_node = (struct node *)stm_allocate(sizeof(struct node),
                                         GCTID_STRUCT_NODE);

    for (i=0; i<UPPER_BOUND; i++) {
        w_node = (struct node *)stm_write_barrier((gcptr)w_node);
        w_node->value = i;
        stm_push_root((gcptr)w_node);
        stm_perform_transaction((gcptr)w_node, insert1);
        stmgcpage_possibly_major_collect(0); /* temp */
        w_node = (struct node *)stm_pop_root();
    }
    stm_finalize();

    status = sem_post(&done);
    assert(status == 0);
    return NULL;
}

void final_check(void)
{
  int seen[UPPER_BOUND];

  stm_initialize();
  do_a_check(seen, 0);
  assert(seen[UPPER_BOUND-1] == NUMTHREADS);
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

int main(void)
{
  int i, status;

  status = sem_init(&done, 0, 0);
  assert(status == 0);

  for (i = 0; i < NUMTHREADS; i++)
    newthread(demo1, NULL);

  for (i=0; i < NUMTHREADS; i++)
    {
      status = sem_wait(&done);
      assert(status == 0);
      printf("thread finished\n");
    }

  final_check();
  return 0;
}
