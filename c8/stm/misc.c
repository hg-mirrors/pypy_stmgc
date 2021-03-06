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
    uint8_t rm = *((char *)(STM_SEGMENT->segment_base + (((uintptr_t)obj) >> 4)));
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
    assert(STM_PSEGMENT->modified_old_objects);
    assert(tree_count(STM_PSEGMENT->modified_old_objects) < 10000);
    return tree_count(STM_PSEGMENT->modified_old_objects);
}

long _stm_count_objects_pointing_to_nursery(void)
{
    if (STM_PSEGMENT->objects_pointing_to_nursery == NULL)
        return -1;
    return list_count(STM_PSEGMENT->objects_pointing_to_nursery);
}

object_t *_stm_enum_modified_old_objects(long index)
{
    wlog_t* entry = tree_item(STM_PSEGMENT->modified_old_objects, index);
    return (object_t*)entry->addr;
}

object_t *_stm_enum_objects_pointing_to_nursery(long index)
{
    return (object_t *)list_item(
        STM_PSEGMENT->objects_pointing_to_nursery, index);
}

static volatile struct stm_commit_log_entry_s *_last_cl_entry;
static long _last_cl_entry_index;
void _stm_start_enum_last_cl_entry()
{
    _last_cl_entry = &commit_log_root;
    volatile struct stm_commit_log_entry_s *cl = (volatile struct stm_commit_log_entry_s *)
        &commit_log_root;

    while ((cl = cl->next)) {
        _last_cl_entry = cl;
    }
    _last_cl_entry_index = 0;
}

object_t *_stm_next_last_cl_entry()
{
    if (_last_cl_entry != &commit_log_root)
        return _last_cl_entry->written[_last_cl_entry_index++];
    return NULL;
}
#endif
