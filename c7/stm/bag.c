/*
Design of stmgc's "bag" objects
===============================

A "bag" is an unordered list of objects.  You can only add objects and
pop a random object.

Conflicts never occur, but popping may return "the bag looks empty",
which can be wrong in the serialized order.  The caller should be
ready to handle this case.  The guarantee is that if you get the
result "the bag looks empty" in all threads that may add objects to
it, and afterwards none of the threads adds any object, then at this
point the bag is really empty.


Implementation
--------------

In raw memory, for each segment, we have a list and a deque:

     abort list             deque
   +--------------+       +-----------------------+---------------+
   | already      |       | next items            | added in this |
   | popped items |       | to pop                | transaction   |
   +--------------+       +-----------------------+---------------+

Adding objects puts them at the right end of the deque.  Popping them
takes them off the left end and stores a copy of the pointer into a
separate list.  This list, the "abort list", is only used to re-add
the objects in case the transaction aborts.

If, when we try to pop, we find that the deque is completely empty,
then we try to "steal" some items from another segment's deque.  This
movement is done completely outside the normal STM rules: the objects
remain moved even after an abort.  More precisely, we take some
objects from the left end of the other segment's deque (but not from
the "added in this transaction" part) and add them to our own deque.
Our own "added in this transaction" part remains empty, and the
objects are not copied in the other transaction's abort list.  This
is done with careful compare-and-swaps.
*/


struct stm_bag_seg_s {
    struct deque_block_s *deque_left, *deque_middle, *deque_right;
    deque_idx_t deque_left_pos, deque_middle_pos, deque_right_pos;
    struct list_s *abort_list;
};

struct stm_bag_s {
    struct stm_bag_seg_s by_segment[STM_NB_SEGMENTS];
};

stm_bag_t *stm_bag_create(void)
{
    int i;
    stm_bag_t *bag = malloc(sizeof(stm_bag_t));
    assert(bag);
    for (i = 0; i < STM_NB_SEGMENTS; i++) {
        struct stm_bag_seg_s *bs = &bag->by_segment[i];
        struct deque_block_s *block = deque_new_block();
        bs->deque_left = block;
        bs->deque_middle = block;
        bs->deque_right = block;
        bs->deque_left_pos = 0;
        bs->deque_middle_pos = 0;
        bs->deque_right_pos = 0;
        LIST_CREATE(bs->abort_list);
    }
    return bag;
}

void stm_bag_free(stm_bag_t *bag)
{
    int i;
    for (i = 0; i < STM_NB_SEGMENTS; i++) {
        struct stm_bag_seg_s *bs = &bag->by_segment[i];
        while (bs->deque_left) {
            struct deque_block_s *block = bs->deque_left;
            bs->deque_left = block->next;
            deque_free_block(block);
        }
        LIST_FREE(bs->abort_list);
    }
}
