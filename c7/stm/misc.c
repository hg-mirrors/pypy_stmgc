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

object_t *_stm_segment_address(char *ptr)
{
    if (ptr == NULL)
        return NULL;

    uintptr_t res = ptr - STM_SEGMENT->segment_base;
    assert(FIRST_OBJECT_PAGE * 4096UL <= res
           && res < NB_PAGES * 4096UL);
    return (object_t*)res;
}

bool _stm_was_read(object_t *obj)
{
    return ((stm_read_marker_t *)(((uintptr_t)obj) >> 4))->rm ==
        STM_SEGMENT->transaction_read_version;
}

bool _stm_was_written(object_t *obj)
{
    return !(obj->stm_flags & GCFLAG_WRITE_BARRIER);
}
