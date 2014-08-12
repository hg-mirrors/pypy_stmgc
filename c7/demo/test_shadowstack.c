#include <stdlib.h>
#include <assert.h>
#include "stmgc.h"

stm_thread_local_t stm_thread_local;

typedef TLPREFIX struct node_s node_t;

struct node_s {
    struct object_s hdr;
    long value;
};

ssize_t stmcb_size_rounded_up(struct object_s *ob)
{
    return sizeof(struct node_s);
}
void stmcb_trace(struct object_s *obj, void visit(object_t **))
{
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


int main(void)
{
    rewind_jmp_buf rjbuf;

    stm_setup();
    stm_register_thread_local(&stm_thread_local);
    stm_rewind_jmp_enterframe(&stm_thread_local, &rjbuf);

    stm_start_transaction(&stm_thread_local);
    node_t *node = (node_t *)stm_allocate(sizeof(struct node_s));
    node->value = 129821;
    STM_PUSH_ROOT(stm_thread_local, node);
    stm_commit_transaction();

    /* now in a new transaction, pop the node off the shadowstack, but
       then do a major collection.  It should still be found by the
       tracing logic. */
    stm_start_transaction(&stm_thread_local);
    STM_POP_ROOT(stm_thread_local, node);
    assert(node->value == 129821);
    STM_PUSH_ROOT(stm_thread_local, NULL);
    stm_collect(9);

    node_t *node2 = (node_t *)stm_allocate(sizeof(struct node_s));
    assert(node2 != node);
    assert(node->value == 129821);

    STM_PUSH_ROOT(stm_thread_local, node2);
    stm_collect(0);
    STM_POP_ROOT(stm_thread_local, node2);
    assert(node2 != node);
    assert(node->value == 129821);

    return 0;
}
