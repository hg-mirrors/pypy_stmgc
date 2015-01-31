/*
Design of stmgc's "bag" objects
===============================

A "bag" is an unordered list of objects.  *This is not a STM-aware
object at all!*  You can add objects and pop a random object.

A typical use case is to collect the objects we want to add in some
regular STM list, and then when we commit, we copy the objects into
the bag.  Note that we *copy*, not *move*: as usual, we must not
change the STM list outside a transaction.

When we pop an object, we should arrange for it to be put back into
the bag if the transaction aborts.


Implementation
--------------

In raw memory, for each segment, we have a list and a deque:

     abort list             deque
   +--------------+       +-----------------------+---------------+
   | already      |       | next items            | added in this |
   | popped items |       | to pop                | transaction   |
   +--------------+       +-----------------------+---------------+
                          ^                       ^               ^
                          left                 middle         right

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


typedef struct bag_node_s {
    struct bag_node_s *next;
    object_t *value;
} bag_node_t;


typedef union {
    /* Data describing the bag from the point of view of segment 'i'. */

    struct {
        bag_node_t *added;    /* added in current transaction */
        bag_node_t *removed;  /* removed in current transaction */

        /* The segment i's transaction's unique_start_time, as it was
           the last time we did a change to this stm_bag_seg_t.  Used
           to detect lazily when a commit occurred in-between. */
        uint64_t start_time;
    };
    char alignment[64];   /* 64-bytes alignment, to prevent false sharing */

} bag_seg_t;


struct stm_bag_s {
    ,,,,,,,,,,,,,,,,,,,,
    bag_node_t *tail;   /* the newest committed element in the bag */

    struct {
    } by_segment[NB_SEGMENTS];
};

stm_bag_t *stm_bag_create(void)
{
    stm_bag_t *bag = malloc(sizeof(stm_bag_t));
    assert(bag);    /* XXX out of memory in stm_bag_create */
    memset(bag, 0, sizeof(stm_bag_t));
    return bag;
}

static void bag_node_free_rec(bag_node_t *p)
{
    while (p != NULL) {
        bag_node_t *q = p->next;
        free(p);
        p = q;
    }
}

void stm_bag_free(stm_bag_t *bag)
{
    int i;
    bag_node_free_rec(bag->tail);
    for (i = 0; i < NB_SEGMENTS; i++) {
        bag_node_free_rec(bag->by_segment[i].added);
        bag_node_free_rec(bag->by_segment[i].removed);
    }
    bag_node_free_rec(bag->head);
    free(bag);
}

void stm_bag_add(stm_bag_t *bag, object_t *newobj)
{
    uint32_t i = STM_SEGMENT->segment_num - 1;
    bag_node_t **p_added = &bag->by_segment[i].added;
    bag_node_t *p = malloc(sizeof(bag_node_t));
    assert(p);       /* XXX */

    p->value = newobj;
    while (1) {
        bag_node_t *old = *p_added;
        p->next = old;
        if (__sync_bool_compare_and_swap(p_added, old, p))
            break;
    }
}

object_t *stm_bag_try_pop(stm_bag_t *bag)
{
    stm_bag_seg_t *bs = bag_check_start_time(bag);

    spinlock_acquire(bs->lock);

    if (bs->deque_left == bs->deque_right) {
        /* look up inside other segments without locks; this might get
           occasional nonsense, but it should not matter here */
        int i;
        stm_bag_seg_t *src = NULL;
        for (i = 0; i < STM_NB_SEGMENTS; i++) {
            stm_bag_seg_t *other = &bag->by_segment[i];
            uintptr_t *left = other->deque_left;
            uintptr_t *middle = other->deque_left;
            ...;
            
            if (other->deque_left != other->deque_right) {
                src = other;
                if (other->deque_
            }
        }

        
        spinlock_release(bs->lock);
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
        spinlock_release(bs->lock);
    }
    else {
        spinlock_release(bs->lock);
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
