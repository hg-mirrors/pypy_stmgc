

void (*stmcb_light_finalizer)(object_t *);

void stm_enable_light_finalizer(object_t *obj)
{
    if (_is_young(obj))
        LIST_APPEND(STM_PSEGMENT->young_objects_with_light_finalizers, obj);
    else
        LIST_APPEND(STM_PSEGMENT->old_objects_with_light_finalizers, obj);
}

static void deal_with_young_objects_with_finalizers(void)
{
    /* for light finalizers */
    struct list_s *lst = STM_PSEGMENT->young_objects_with_light_finalizers;
    long i, count = list_count(lst);
    for (i = 0; i < count; i++) {
        object_t* obj = (object_t *)list_item(lst, i);
        assert(_is_young(obj));

        object_t *TLPREFIX *pforwarded_array = (object_t *TLPREFIX *)obj;
        if (pforwarded_array[0] != GCWORD_MOVED) {
            /* not moved: the object dies */
            stmcb_light_finalizer(obj);
        }
        else {
            obj = pforwarded_array[1]; /* moved location */
            assert(!_is_young(obj));
            LIST_APPEND(STM_PSEGMENT->old_objects_with_light_finalizers, obj);
        }
    }
    list_clear(lst);
}

static void deal_with_old_objects_with_finalizers(void)
{
    /* for light finalizers */
    long j;
    for (j = 1; j <= NB_SEGMENTS; j++) {
        struct stm_priv_segment_info_s *pseg = get_priv_segment(j);

        struct list_s *lst = pseg->old_objects_with_light_finalizers;
        long i, count = list_count(lst);
        lst->count = 0;
        for (i = 0; i < count; i++) {
            object_t* obj = (object_t *)list_item(lst, i);
            if (!mark_visited_test(obj)) {
                /* not marked: object dies */
                /* we're calling the light finalizer is a random thread,
                   but it should work, because it was dead already at the
                   start of that thread's transaction, so any thread should
                   see the same, old content */
                stmcb_light_finalizer(obj);
            }
            else {
                /* object survives */
                list_set_item(lst, lst->count++, (uintptr_t)obj);
            }
        }
    }
}


/************************************************************/
/*  Algorithm for regular (non-light) finalizers.
    Follows closely pypy/doc/discussion/finalizer-order.rst
    as well as rpython/memory/gc/minimark.py.
*/

static inline int _finalization_state(object_t *obj)
{
    /* Returns the state, "0", 1, 2 or 3, as per finalizer-order.rst.
       One difference is that the official state 0 is returned here
       as a number that is <= 0. */
    uintptr_t lock_idx = mark_loc(obj);
    return write_locks[lock_idx] - (WL_FINALIZ_ORDER_1 - 1);
}

static void _bump_finalization_state_from_0_to_1(object_t *obj)
{
    uintptr_t lock_idx = mark_loc(obj);
    assert(write_locks[lock_idx] < WL_FINALIZ_ORDER_1);
    write_locks[lock_idx] = WL_FINALIZ_ORDER_1;
}

static struct list_s *_finalizer_tmpstack, *_finalizer_emptystack;

static inline void _append_to_finalizer_tmpstack(object_t **pobj)
{
    object_t *obj = *pobj;
    if (obj != NULL)
        LIST_APPEND(_finalizer_tmpstack, obj);
}

static inline struct list_s *finalizer_trace(object_t *obj, struct list_s *lst)
{
    struct object_s *realobj =
        (struct object_s *)REAL_ADDRESS(stm_object_pages, obj);
    _finalizer_tmpstack = lst;
    stmcb_trace(realobj, &_append_to_finalizer_tmpstack);
    return _finalizer_tmpstack;
}

static void _recursively_bump_finalization_state(object_t *obj, int from_state)
{
    struct list_s *tmpstack = _finalizer_emptystack;
    assert(list_is_empty(tmpstack));

    assert(_finalization_state(obj) == from_state);

    while (1) {
        if (_finalization_state(obj) == from_state) {
            /* bump to the next state */
            write_locks[mark_loc(obj)]++;

            /* trace */
            tmpstack = finalizer_trace(obj, tmpstack);
        }

        if (list_is_empty(tmpstack))
            break;

        obj = (object_t *)list_pop_item(tmpstack);
    }
    _finalizer_emptystack = tmpstack;
}

static void deal_with_objects_with_finalizers(void)
{
    /* for non-light finalizers */

    /* there is one 'objects_with_finalizers' list per segment, but it
       doesn't really matter: all objects are considered equal, and if
       they survive, they are added again into one list that is attached
       at the end to an arbitrary segment. */
    struct list_s *new_with_finalizer = list_create();
    struct list_s *marked = list_create();
    struct list_s *pending = list_create();
    LIST_CREATE(_finalizer_emptystack);

    long j;
    for (j = 1; j <= NB_SEGMENTS; j++) {
        struct stm_priv_segment_info_s *pseg = get_priv_segment(j);

        struct list_s *lst = pseg->objects_with_finalizers;
        long i, count = list_count(lst);
        for (i = 0; i < count; i++) {
            object_t *x = (object_t *)list_item(lst, i);

            assert(_finalization_state(x) != 1);
            if (_finalization_state(x) >= 2) {
                LIST_APPEND(new_with_finalizer, x);
                continue;
            }
            LIST_APPEND(marked, x);
            LIST_APPEND(pending, x);
            while (!list_is_empty(pending)) {
                object_t *y = (object_t *)list_pop_item(pending);
                int state = _finalization_state(y);
                if (state <= 0) {
                    _bump_finalization_state_from_0_to_1(y);
                    pending = finalizer_trace(y, pending);
                }
                else if (state == 2) {
                    _recursively_bump_finalization_state(y, 2);
                }
            }
            _recursively_bump_finalization_state(x, 1);
        }
        list_clear(lst);
    }

    long i, count = list_count(marked);
    for (i = 0; i < count; i++) {
        object_t *x = (object_t *)list_item(marked, i);

        int state = _finalization_state(x);
        assert(state >= 2);
        if (state == 2) {
            LIST_APPEND(run_finalizers, x);
            _recursively_bump_finalization_state(x, 2);
        }
        else {
            LIST_APPEND(new_with_finalizer, x);
        }
    }

    LIST_FREE(_finalizer_emptystack);
    list_free(pending);
    list_free(marked);
    list_free(get_priv_segment(1)->objects_with_finalizers);
    get_priv_segment(1)->objects_with_finalizers = new_with_finalizer;
}
