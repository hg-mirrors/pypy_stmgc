#ifndef _STM_CORE_H_
# error "must be compiled via stmgc.c"
#endif


char *_stm_real_address(object_t *o)
{
    if (o == NULL)
        return NULL;

    assert(FIRST_OBJECT_PAGE * 4096UL <= (uintptr_t)o
           && (uintptr_t)o < NB_PAGES * 4096UL);
    return REAL_ADDRESS(STM_SEGMENT->segment_base, o);
}

char *_stm_get_segment_base(long index)
{
    return get_segment_base(index);
}

struct stm_priv_segment_info_s *_stm_segment(void)
{
    char *info = REAL_ADDRESS(STM_SEGMENT->segment_base, STM_PSEGMENT);
    return (struct stm_priv_segment_info_s *)info;
}

stm_thread_local_t *_stm_thread(void)
{
    return STM_SEGMENT->running_thread;
}

bool _stm_was_read(object_t *obj)
{
    uint8_t rm = ((struct stm_read_marker_s *)
                  (STM_SEGMENT->segment_base + (((uintptr_t)obj) >> 4)))->rm;
    assert(rm <= STM_SEGMENT->transaction_read_version);
    return rm == STM_SEGMENT->transaction_read_version;
}

bool _stm_was_written(object_t *obj)
{
    return (obj->stm_flags & _STM_GCFLAG_WRITE_BARRIER) == 0;
}


#ifdef STM_TESTS

long _stm_count_modified_old_objects(void)
{
    if (STM_PSEGMENT->modified_old_objects == NULL)
        return -1;
    return list_count(STM_PSEGMENT->modified_old_objects);
}


object_t *_stm_enum_modified_old_objects(long index)
{
    return (object_t *)list_item(
        STM_PSEGMENT->modified_old_objects, index);
}
#endif
