#ifndef _STM_CORE_H_
# error "must be compiled via stmgc.c"
#endif


#define GCWORD_PREBUILT_MOVED  ((object_t *) 42)


object_t *stm_setup_prebuilt(object_t *staticobj_invalid)
{
    /* All variable names in "_invalid" here mean that although the
       type is really "object_t *", it should not actually be accessed
       via %gs.

       If the object was already moved, its first word was set to
       GCWORD_PREBUILT_MOVED.  In that case, the forwarding location,
       i.e. where the object moved to, is stored in the second word.
    */
    uintptr_t objaddr = (uintptr_t)staticobj_invalid;
    struct object_s *obj = (struct object_s *)objaddr;
    object_t **pforwarded_array = (object_t **)objaddr;

    if (pforwarded_array[0] == GCWORD_PREBUILT_MOVED) {
        return pforwarded_array[1];    /* already moved */
    }

    /* We need to make a copy of this object. */
    size_t size = stmcb_size_rounded_up(obj);
    object_t *nobj = _stm_allocate_old(size);

    /* Copy the object */
    char *realnobj = REAL_ADDRESS(stm_object_pages, nobj);
    memcpy(realnobj, (char *)objaddr, size);

    // XXX REFERENCES HERE

    return nobj;
}
