#ifndef _STM_CORE_H_
# error "must be compiled via stmgc.c"
#endif


stm_char *_stm_allocate_slowpath(ssize_t size_rounded_up)
{
    abort();
}


object_t *stm_allocate_prebuilt(ssize_t size_rounded_up)
{
    abort();
}
