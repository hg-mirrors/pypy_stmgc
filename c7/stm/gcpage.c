#ifndef _STM_CORE_H_
# error "must be compiled via stmgc.c"
#endif


static void setup_gcpage(void)
{
    largemalloc_init_arena(stm_object_pages + END_NURSERY_PAGE * 4096UL,
                           (NB_PAGES - END_NURSERY_PAGE) * 4096UL);
}

object_t *_stm_allocate_old(ssize_t size_rounded_up)
{
    char *addr = large_malloc(size_rounded_up);
    object_t* o = (object_t *)(addr - stm_object_pages);

    memset(REAL_ADDRESS(STM_SEGMENT->segment_base, o), 0, size_rounded_up);
    o->stm_flags = GCFLAG_WRITE_BARRIER;
    return o;
}
