#ifndef _STM_CORE_H_
# error "must be compiled via stmgc.c"
#endif


object_t *stm_allocate_weakref(ssize_t size_rounded_up)
{
    OPT_ASSERT(size_rounded_up > sizeof(struct object_s));
    object_t *obj = stm_allocate(size_rounded_up);

    assert(_is_in_nursery(obj)); /* see assert(0) which depends on it */

    LIST_APPEND(STM_PSEGMENT->young_weakrefs, obj);
    return obj;
}


/***** Minor collection *****/

void stm_move_young_weakrefs()
{
    /* The code relies on the fact that no weakref can be an old object
       weakly pointing to a young object.  Indeed, weakrefs are immutable
       so they cannot point to an object that was created after it.
    */
    LIST_FOREACH_R(
        STM_PSEGMENT->young_weakrefs,
        object_t * /*item*/,
        ({
            if (_is_in_nursery(item)) {
                object_t *TLPREFIX *pforwarded_array = (object_t *TLPREFIX *)item;

                /* the following checks are done like in nursery.c: */
                if (!(item->stm_flags & GCFLAG_HAS_SHADOW)
                    || (pforwarded_array[0] != GCWORD_MOVED)) {
                    /* weakref dies */
                    continue;
                }

                item = pforwarded_array[1]; /* moved location */
            }
            else {
                /* tell me if we need this (requires synchronizing in case
                   of private pages) */
                assert(0);
                /* /\* young outside nursery object *\/ */
                /* if (tree_contains(STM_PSEGMENT->young_outside_nursery, */
                /*                   (uintptr_t)item)) { */
                /*     /\* still in the tree -> wasn't seen by the minor collection, */
                /*        so it doesn't survive *\/ */
                /*     continue; */
                /* } */
            }
            assert(!_is_young(item));

            char *realobj = REAL_ADDRESS(STM_SEGMENT->segment_base, item);
            ssize_t size = stmcb_size_rounded_up((struct object_s *)realobj);
            object_t *pointing_to = *WEAKREF_PTR(item, size);
            assert(pointing_to != NULL);

            if (_is_in_nursery(pointing_to)) {
                object_t *TLPREFIX *pforwarded_array = (object_t *TLPREFIX *)pointing_to;
                /* the following checks are done like in nursery.c: */
                if (!(pointing_to->stm_flags & GCFLAG_HAS_SHADOW)
                    || (pforwarded_array[0] != GCWORD_MOVED)) {
                    /* pointing_to dies */
                    *WEAKREF_PTR(item, size) = NULL;
                    synchronize_overflow_object_now(item);
                    continue;   /* no need to remember in old_weakrefs */
                }
                else {
                    /* moved location */
                    *WEAKREF_PTR(item, size) = pforwarded_array[1];
                    synchronize_overflow_object_now(item);
                }
            }
            else {
                /* young outside nursery object or old object */
                if (tree_contains(STM_PSEGMENT->young_outside_nursery,
                                  (uintptr_t)pointing_to)) {
                    /* still in the tree -> wasn't seen by the minor collection,
                       so it doesn't survive */
                    *WEAKREF_PTR(item, size) = NULL;
                    synchronize_overflow_object_now(item);
                    continue;   /* no need to remember in old_weakrefs */
                }
                /* pointing_to was already old */
            }
            LIST_APPEND(STM_PSEGMENT->old_weakrefs, item);
        }));
    list_clear(STM_PSEGMENT->young_weakrefs);
}


/***** Major collection *****/


void stm_visit_old_weakrefs(void)
{
    long i;
    for (i = 0; i < NB_SEGMENTS; i++) {
        struct stm_priv_segment_info_s *pseg = get_priv_segment(i);
        struct list_s *lst;

        lst = pseg->old_weakrefs;
        uintptr_t n = list_count(lst);
        while (n > 0) {
            object_t *weakref = (object_t *)list_item(lst, --n);
            if (!mark_visited_test(weakref)) {
                /* weakref dies */
                list_set_item(lst, n, list_pop_item(lst));
                continue;
            }

            char *realobj = REAL_ADDRESS(pseg->pub.segment_base, weakref);
            ssize_t size = stmcb_size_rounded_up((struct object_s *)realobj);
            object_t *pointing_to = *WEAKREF_PTR(weakref, size);
            assert(pointing_to != NULL);
            if (!mark_visited_test(pointing_to)) {
                //assert(flag_page_private[(uintptr_t)weakref / 4096UL] != PRIVATE_PAGE);
                *WEAKREF_PTR(weakref, size) = NULL;
                if (flag_page_private[(uintptr_t)weakref / 4096UL] == PRIVATE_PAGE) {
                    synchronize_overflow_object_now(weakref);
                }

                /* we don't need it in this list anymore */
                list_set_item(lst, n, list_pop_item(lst));
                continue;
            }
            else {
                /* it survives! */
            }
        }
    }
}
