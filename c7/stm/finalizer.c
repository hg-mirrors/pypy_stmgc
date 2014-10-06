

void (*stmcb_light_finalizer)(object_t *);

void stm_enable_light_finalizer(object_t *obj)
{
    if (_is_young(obj)) {
        STM_PSEGMENT->young_objects_with_light_finalizers = list_append(
            STM_PSEGMENT->young_objects_with_light_finalizers, (uintptr_t)obj);
    }
    else {
        STM_PSEGMENT->old_objects_with_light_finalizers = list_append(
            STM_PSEGMENT->old_objects_with_light_finalizers, (uintptr_t)obj);
    }
}

static void deal_with_young_objects_with_finalizers(void)
{
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
            STM_PSEGMENT->old_objects_with_light_finalizers = list_append(
               STM_PSEGMENT->old_objects_with_light_finalizers, (uintptr_t)obj);
        }
    }
    list_clear(lst);
}

static void deal_with_old_objects_with_finalizers(void)
{
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
