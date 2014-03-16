#ifndef _STM_CORE_H_
# error "must be compiled via stmgc.c"
#endif

#define WEAKREF_PTR(wr, sz)  ((object_t * TLPREFIX *)(((stm_char *)(wr)) + (sz) - sizeof(void*)))

object_t *stm_allocate_weakref(ssize_t size_rounded_up)
{
    OPT_ASSERT(size_rounded_up > sizeof(struct object_s));
    OPT_ASSERT(size_rounded_up == 16); /* no reason for it to be anything else */

    object_t *obj = stm_allocate(size_rounded_up);
    assert(_is_in_nursery(obj)); /* because it's so small */

    LIST_APPEND(STM_PSEGMENT->young_weakrefs, obj);
    return obj;
}


object_t *stm_setup_prebuilt_weakref(object_t *obj)
{
    ssize_t size = 16;

    obj = stm_setup_prebuilt(obj);
    *WEAKREF_PTR(obj, size) = stm_setup_prebuilt(*WEAKREF_PTR(obj, size));
    return obj;
}


static void _set_weakref_in_all_segments(object_t *weakref, object_t *value)
{
    abort();//XXX
#if 0
    ssize_t size = 16;

    stm_char *point_to_loc = (stm_char*)WEAKREF_PTR(weakref, size);
    if (flag_page_private[(uintptr_t)point_to_loc / 4096UL] == PRIVATE_PAGE) {
        long i;
        for (i = 0; i < NB_SEGMENTS; i++) {
            char *base = get_segment_base(i);   /* two different segments */

            object_t ** ref_loc = (object_t **)REAL_ADDRESS(base, point_to_loc);
            *ref_loc = value;
        }
    }
    else {
        *WEAKREF_PTR(weakref, size) = value;
    }
#endif
}

/***** Minor collection *****/

static void stm_move_young_weakrefs()
{
    /* The code relies on the fact that no weakref can be an old object
       weakly pointing to a young object.  Indeed, weakrefs are immutable
       so they cannot point to an object that was created after it.
    */
    LIST_FOREACH_R(
        STM_PSEGMENT->young_weakrefs,
        object_t * /*item*/,
        ({
            /* weakrefs are so small, they always are in the nursery. Never
               a young outside nursery object. */
            assert(_is_in_nursery(item));
            object_t *TLPREFIX *pforwarded_array = (object_t *TLPREFIX *)item;

            /* the following checks are done like in nursery.c: */
            if (!(item->stm_flags & GCFLAG_HAS_SHADOW)
                || (pforwarded_array[0] != GCWORD_MOVED)) {
                /* weakref dies */
                continue;
            }

            item = pforwarded_array[1]; /* moved location */

            assert(!_is_young(item));

            ssize_t size = 16;
            object_t *pointing_to = *WEAKREF_PTR(item, size);
            assert(pointing_to != NULL);

            if (_is_in_nursery(pointing_to)) {
                object_t *TLPREFIX *pforwarded_array = (object_t *TLPREFIX *)pointing_to;
                /* the following checks are done like in nursery.c: */
                if (!(pointing_to->stm_flags & GCFLAG_HAS_SHADOW)
                    || (pforwarded_array[0] != GCWORD_MOVED)) {
                    /* pointing_to dies */
                    _set_weakref_in_all_segments(item, NULL);
                    continue;   /* no need to remember in old_weakrefs */
                }
                else {
                    /* moved location */
                    _set_weakref_in_all_segments(item, pforwarded_array[1]);
                }
            }
            else {
                /* young outside nursery object or old object */
                if (tree_contains(STM_PSEGMENT->young_outside_nursery,
                                  (uintptr_t)pointing_to)) {
                    /* still in the tree -> wasn't seen by the minor collection,
                       so it doesn't survive */
                    _set_weakref_in_all_segments(item, NULL);
                    continue;   /* no need to remember in old_weakrefs */
                }
                /* pointing_to was already old */
            }
            LIST_APPEND(STM_PSEGMENT->old_weakrefs, item);
        }));
    list_clear(STM_PSEGMENT->young_weakrefs);
}


/***** Major collection *****/


static void stm_visit_old_weakrefs(void)
{
    long i;
    for (i = 1; i <= NB_SEGMENTS; i++) {
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

            ssize_t size = 16;
            object_t *pointing_to = *WEAKREF_PTR(weakref, size);
            assert(pointing_to != NULL);
            if (!mark_visited_test(pointing_to)) {
                //assert(flag_page_private[(uintptr_t)weakref / 4096UL] != PRIVATE_PAGE);
                _set_weakref_in_all_segments(weakref, NULL);

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
