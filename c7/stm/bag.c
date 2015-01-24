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


typedef union {
    struct {
        uintptr_t *deque_left, *deque_middle, *deque_right;
        struct list_s *abort_list;
        uint64_t start_time;    /* the transaction's unique_start_time */
        bool must_add_to_modified_bags;
    };
    char alignment[64];   /* 64-bytes alignment, to prevent false sharing */
} stm_bag_seg_t;

struct stm_bag_s {
    stm_bag_seg_t by_segment[STM_NB_SEGMENTS];
};

stm_bag_t *stm_bag_create(void)
{
    int i;
    stm_bag_t *bag;
    void *mem;

    assert(sizeof(stm_bag_seg_t) == 64);
    if (posix_memalign(&mem, sizeof(stm_bag_seg_t), sizeof(stm_bag_t)) != 0)
        stm_fatalerror("out of memory in stm_bag_create");   /* XXX */

    bag = (stm_bag_t *)mem;
    for (i = 0; i < STM_NB_SEGMENTS; i++) {
        stm_bag_seg_t *bs = &bag->by_segment[i];
        struct deque_block_s *block = deque_new_block();
        bs->deque_left = &block->items[0];
        bs->deque_middle = &block->items[0];
        bs->deque_right = &block->items[0];
        LIST_CREATE(bs->abort_list);
        bs->start_time = 0;
        bs->must_add_to_modified_bags = false;   /* currently young */
    }
    return bag;
}

void stm_bag_free(stm_bag_t *bag)
{
    int i;

    s_mutex_lock();
    for (i = 0; i < STM_NB_SEGMENTS; i++) {
        stm_bag_seg_t *bs = &bag->by_segment[i];
        struct stm_segment_info_s *pub = get_segment(i + 1);
        stm_thread_local_t *tl = pub->running_thread;
        if (tl != NULL && tl->associated_segment_num == i + 1) {
            stm_call_on_abort(tl, bs, NULL);
        }
    }
    s_mutex_unlock();

    for (i = 0; i < STM_NB_SEGMENTS; i++) {
        stm_bag_seg_t *bs = &bag->by_segment[i];
        struct deque_block_s *block = deque_block(bs->deque_left);
        while (block != NULL) {
            struct deque_block_s *nextblock = block->next;
            deque_free_block(block);
            block = nextblock;
        }
        LIST_FREE(bs->abort_list);
    }

    free(bag);
}

static void bag_add(stm_bag_seg_t *bs, object_t *newobj)
{
    struct deque_block_s *block = deque_block(bs->deque_right);
    *bs->deque_right++ = (uintptr_t)newobj;

    if (bs->deque_right == &block->items[DEQUE_BLOCK_SIZE]) {
        if (block->next == NULL)
            block->next = deque_new_block();
        bs->deque_right = &block->next->items[0];
    }
}

static void bag_abort_callback(void *key)
{
    stm_bag_seg_t *bs = (stm_bag_seg_t *)key;

    /* remove the "added in this transaction" items */
    bs->deque_right = bs->deque_middle;

    /* reinstall the items from the "abort_list" */
    LIST_FOREACH_F(bs->abort_list, object_t *, bag_add(bs, item));
    list_clear(bs->abort_list);

    /* these items are not "added in this transaction" */
    bs->deque_middle = bs->deque_right;
}

static stm_bag_seg_t *bag_check_start_time(stm_bag_t *bag)
{
    int i = STM_SEGMENT->segment_num - 1;
    stm_bag_seg_t *bs = &bag->by_segment[i];

    if (bs->start_time != STM_PSEGMENT->unique_start_time) {
        /* There was a commit or an abort since the last operation
           on the same bag in the same segment.  If there was an
           abort, bag_abort_callback() should have been called to
           reset the state.  Assume that any non-reset state is
           there because of a commit.

           The middle pointer moves to the right: there are no
           more "added in this transaction" entries.  And the
           "already popped items" list is forgotten.
        */
        bs->deque_middle = bs->deque_right;
        list_clear(bs->abort_list);
        bs->start_time = STM_PSEGMENT->unique_start_time;
        bs->must_add_to_modified_bags = true;

        /* We're about to modify the bag, so register an abort
           callback now. */
        stm_thread_local_t *tl = STM_SEGMENT->running_thread;
        assert(tl->associated_segment_num == STM_SEGMENT->segment_num);
        stm_call_on_abort(tl, bs, &bag_abort_callback);
    }

    return bs;
}

void stm_bag_add(stm_bag_t *bag, object_t *newobj)
{
    stm_bag_seg_t *bs = bag_check_start_time(bag);
    bag_add(bs, newobj);

    if (bs->must_add_to_modified_bags) {
        bs->must_add_to_modified_bags = false;
        if (STM_PSEGMENT->modified_bags == NULL)
            LIST_CREATE(STM_PSEGMENT->modified_bags);
        LIST_APPEND(STM_PSEGMENT->modified_bags, bag);
    }
}

object_t *stm_bag_try_pop(stm_bag_t *bag)
{
    stm_bag_seg_t *bs = bag_check_start_time(bag);
    if (bs->deque_left == bs->deque_right) {
        return NULL;
    }

    struct deque_block_s *block = deque_block(bs->deque_left);
    bool from_same_transaction = (bs->deque_left == bs->deque_middle);
    uintptr_t result = *bs->deque_left++;

    if (bs->deque_left == &block->items[DEQUE_BLOCK_SIZE]) {
        bs->deque_left = &block->next->items[0];
        deque_free_block(block);
    }
    if (from_same_transaction) {
        bs->deque_middle = bs->deque_left;
    }
    else {
        LIST_APPEND(bs->abort_list, result);
    }
    return (object_t *)result;
}

void stm_bag_tracefn(stm_bag_t *bag, void trace(object_t **))
{
    if (trace == TRACE_FOR_MINOR_COLLECTION) {
        /* only trace the items added in the current transaction;
           the rest is already old and cannot point into the nursery. */
        int i = STM_SEGMENT->segment_num - 1;
        stm_bag_seg_t *bs = &bag->by_segment[i];

        deque_trace(bs->deque_middle, bs->deque_right, trace);

        bs->must_add_to_modified_bags = true;
    }
    else {
        int i;
        for (i = 0; i < NB_SEGMENTS; i++) {
            stm_bag_seg_t *bs = &bag->by_segment[i];
            deque_trace(bs->deque_left, bs->deque_right, trace);
        }
    }
}

static void collect_modified_bags(void)
{
    LIST_FOREACH_R(STM_PSEGMENT->modified_bags, stm_bag_t *,
                   stm_bag_tracefn(item, TRACE_FOR_MINOR_COLLECTION));
    LIST_FREE(STM_PSEGMENT->modified_bags);
}
