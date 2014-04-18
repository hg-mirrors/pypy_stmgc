#ifndef _STM_CORE_H_
# error "must be compiled via stmgc.c"
#endif


void (*stmcb_expand_marker)(uintptr_t odd_number,
                            object_t *following_object,
                            char *outputbuf, size_t outputbufsize);


static void marker_fetch_expand(struct stm_priv_segment_info_s *pseg)
{
    pseg->marker_self[0] = 0;

    if (stmcb_expand_marker != NULL) {
        stm_thread_local_t *tl = pseg->pub.running_thread;
        struct stm_shadowentry_s *current = tl->shadowstack - 1;
        struct stm_shadowentry_s *base = tl->shadowstack_base;
        while (--current >= base) {
            uintptr_t x = (uintptr_t)current->ss;
            if (x & 1) {
                /* the stack entry is an odd number */
                stmcb_expand_marker(x, current[1].ss,
                                    pseg->marker_self, _STM_MARKER_LEN);
                break;
            }
        }
    }
}

static void marker_copy(stm_thread_local_t *tl,
                        struct stm_priv_segment_info_s *pseg,
                        enum stm_time_e attribute_to, double time)
{
    /* Copies the marker information from pseg to tl.  This is called
       indirectly from abort_with_mutex(), but only if the lost time is
       greater than that of the previous recorded marker.  By contrast,
       pseg->marker_self has been filled already in all cases.  The
       reason for the two steps is that we must fill pseg->marker_self
       earlier than now (some objects may be GCed), but we only know
       here the total time it gets attributed.
    */
    tl->longest_marker_state = attribute_to;
    tl->longest_marker_time = time;
    memcpy(tl->longest_marker_self, pseg->marker_self, _STM_MARKER_LEN);
}
