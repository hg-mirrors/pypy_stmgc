#ifndef _STM_CORE_H_
# error "must be compiled via stmgc.c"
#endif


void (*stmcb_expand_marker)(uintptr_t odd_number,
                            object_t *following_object,
                            char *outputbuf, size_t outputbufsize);


void marker_fetch(stm_thread_local_t *tl,
                  enum stm_time_e attribute_to, double time)
{
    tl->longest_marker_state = attribute_to;
    tl->longest_marker_time = time;

    if (stmcb_expand_marker != NULL) {
        struct stm_shadowentry_s *current = tl->shadowstack - 1;
        struct stm_shadowentry_s *base = tl->shadowstack_base;
        while (--current >= base) {
            uintptr_t x = (uintptr_t)current->ss;
            if (x & 1) {
                /* the stack entry is an odd number */
                tl->longest_marker_self[0] = 0;
                stmcb_expand_marker(x, current[1].ss,
                                    tl->longest_marker_self,
                                    sizeof(tl->longest_marker_self));
                break;
            }
        }
    }
}
