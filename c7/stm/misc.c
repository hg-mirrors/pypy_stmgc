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
    return ((stm_read_marker_t *)(((uintptr_t)obj) >> 4))->rm ==
        STM_SEGMENT->transaction_read_version;
}

bool _stm_was_written(object_t *obj)
{
    return !!((((stm_creation_marker_t *)(((uintptr_t)obj) >> 8))->cm |
               obj->stm_flags) & _STM_GCFLAG_WRITE_BARRIER_CALLED);
}

static inline bool was_read_remote(char *base, object_t *obj,
                                   uint8_t other_transaction_read_version)
{
    struct read_marker_s *marker = (struct read_marker_s *)
        (base + (((uintptr_t)obj) >> 4));
    return (marker->rm == other_transaction_read_version);
}
