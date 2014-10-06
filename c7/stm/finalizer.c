

void (*stmcb_light_finalizer)(object_t *);

void stm_enable_light_finalizer(object_t *obj)
{
    STM_PSEGMENT->young_objects_with_light_finalizers = list_append(
        STM_PSEGMENT->young_objects_with_light_finalizers, (uintptr_t)obj);
}

static void deal_with_young_objects_with_finalizers(void)
{
    struct list_s *lst = STM_PSEGMENT->young_objects_with_light_finalizers;
    long i, count = list_count(lst);
    for (i = 0; i < count; i++) {
        object_t* obj = (object_t *)list_item(lst, i);
        stmcb_light_finalizer(obj);
    }
    list_clear(lst);
}
