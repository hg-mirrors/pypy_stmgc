#ifndef _STM_CORE_H_
# error "must be compiled via stmgc.c"
#endif


static uint64_t prebuilt_readmarkers_start = 0;
static uint64_t prebuilt_readmarkers_end   = 0;
static uint64_t prebuilt_objects_start     = 0;


/* XXX NOT TESTED, AND NOT WORKING RIGHT NOW */

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

    uint64_t utarget = (uint64_t)target;
    uint64_t rm_start = utarget / 16;
    uint64_t rm_end   = (utarget + size + 15) / 16;

    if (rm_start < 8192 || rm_end > (utarget & ~4095) ||
            utarget + size > FIRST_READMARKER_PAGE * 4096UL) {
        fprintf(stderr,
                "stm_copy_prebuilt_objects: invalid range (0x%lx, 0x%lx)\n",
                (long)utarget, (long)size);
        abort();
    }

    if (prebuilt_readmarkers_start == 0) {
        prebuilt_readmarkers_start = rm_start;
        prebuilt_readmarkers_end   = rm_end;
        prebuilt_objects_start     = utarget & ~4095;
    }
    else {
        if (prebuilt_readmarkers_start > rm_start)
            prebuilt_readmarkers_start = rm_start;
        if (prebuilt_readmarkers_end < rm_end)
            prebuilt_readmarkers_end = rm_end;
        if (prebuilt_objects_start > (utarget & ~4095))
            prebuilt_objects_start = utarget & ~4095;

        if (prebuilt_readmarkers_end > prebuilt_objects_start) {
            fprintf(stderr,
                    "stm_copy_prebuilt_objects: read markers ending at 0x%lx "
                    "overlap with prebuilt objects starting at 0x%lx\n",
                    (long)prebuilt_readmarkers_end,
                    (long)prebuilt_objects_start);
            abort();
        }
    }

    uint64_t start_page = utarget / 4096;
    uint64_t end_page = (utarget + size + 4095) / 4096;
    pages_initialize_shared(start_page, end_page - start_page);

    char *segment_base = get_segment_base(0);
    memcpy(REAL_ADDRESS(segment_base, utarget), source, size);
}

#if 0
static void reset_transaction_read_version_prebuilt(void)
{
    memset(REAL_ADDRESS(STM_SEGMENT->segment_base, prebuilt_readmarkers_start),
           0, prebuilt_readmarkers_end - prebuilt_readmarkers_start);
}
#endif
