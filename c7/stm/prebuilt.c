#ifndef _STM_CORE_H_
# error "must be compiled via stmgc.c"
#endif


void stm_copy_prebuilt_objects(object_t *target, char *source, ssize_t size)
{
    /* Initialize a region of 'size' bytes at the 'target' address,
       containing prebuilt objects copied from 'source'.  The caller
       must ensure that the 'target' address is valid.  It might be
       called several times but care must be taken not to overlap the
       ranges.  The exact rules are a bit complicated:

       - the range [target, target + size] must be inside the
         range [131072, FIRST_READMARKER_PAGE*4096]

       - the range [target / 16, (target + size) / 16] will be
         used by read markers, so it must be fully before the
         range [target, target + size].

       The objects themselves can contain more pointers to other
       prebuilt objects.  Their stm_flags field must be initialized
       with STM_FLAGS_PREBUILT.
    */

    uintptr_t utarget = (uintptr_t)target;
    if (utarget / 16 < 8192 ||
            utarget + size > FIRST_READMARKER_PAGE * 4096UL ||
            (utarget + size + 15) / 16 > utarget) {
        fprintf(stderr,
                "stm_copy_prebuilt_objects: invalid range (%ld, %ld)",
                (long)utarget, (long)size);
        abort();
    }
    uintptr_t start_page = utarget / 4096;
    uintptr_t end_page = (utarget + size + 4095) / 4096;
    pages_initialize_shared(start_page, end_page - start_page);

    char *segment_base = get_segment_base(0);
    memcpy(REAL_ADDRESS(segment_base, utarget), source, size);
}
