#ifndef _STM_CORE_H_
# error "must be compiled via stmgc.c"
#endif


void (*stmcb_expand_marker)(char *segment_base, uintptr_t odd_number,
                            object_t *following_object,
                            char *outputbuf, size_t outputbufsize);

void (*stmcb_debug_print)(const char *cause, double time,
                          const char *marker);


static void marker_fetch(stm_thread_local_t *tl, uintptr_t marker[2])
{
    struct stm_shadowentry_s *current = tl->shadowstack - 1;
    struct stm_shadowentry_s *base = tl->shadowstack_base;
    /* stop walking just before shadowstack_base, which contains
       STM_STACK_MARKER_OLD which shouldn't be expanded */
    while (--current > base) {
        if (((uintptr_t)current->ss) & 1) {
            /* found the odd marker */
            marker[0] = (uintptr_t)current[0].ss;
            marker[1] = (uintptr_t)current[1].ss;
            return;
        }
    }
    marker[0] = 0;
    marker[1] = 0;
}

static void marker_expand(uintptr_t marker[2], char *segment_base,
                          char *outmarker)
{
    outmarker[0] = 0;
    if (marker[0] == 0)
        return;   /* no marker entry found */
    if (stmcb_expand_marker != NULL) {
        stmcb_expand_marker(segment_base, marker[0], (object_t *)marker[1],
                            outmarker, _STM_MARKER_LEN);
    }
}

static void marker_fetch_expand(struct stm_priv_segment_info_s *pseg)
{
    if (pseg->marker_self[0] != 0)
        return;   /* already collected an entry */

    uintptr_t marker[2];
    marker_fetch(pseg->pub.running_thread, marker);
    marker_expand(marker, pseg->pub.segment_base, pseg->marker_self);
    pseg->marker_other[0] = 0;
}

char *_stm_expand_marker(void)
{
    /* for tests only! */
    static char _result[_STM_MARKER_LEN];
    uintptr_t marker[2];
    _result[0] = 0;
    marker_fetch(STM_SEGMENT->running_thread, marker);
    marker_expand(marker, STM_SEGMENT->segment_base, _result);
    return _result;
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
    if (stmcb_debug_print) {
        stmcb_debug_print(timer_names[attribute_to], time, pseg->marker_self);
    }
    if (time * 0.99 > tl->longest_marker_time) {
        tl->longest_marker_state = attribute_to;
        tl->longest_marker_time = time;
        memcpy(tl->longest_marker_self, pseg->marker_self, _STM_MARKER_LEN);
        memcpy(tl->longest_marker_other, pseg->marker_other, _STM_MARKER_LEN);
    }
    pseg->marker_self[0] = 0;
    pseg->marker_other[0] = 0;
}

static void marker_fetch_obj_write(uint8_t in_segment_num, object_t *obj,
                                   uintptr_t marker[2])
{
    char *segment_base = get_segment_base(in_segment_num);
    acquire_marker_lock(segment_base);
    assert(_has_mutex());

    /* here, we acquired the other thread's marker_lock, which means that:

       (1) it has finished filling 'modified_old_objects' after it sets
           up the write_locks[] value that we're conflicting with

       (2) it is not mutating 'modified_old_objects' right now (we have
           the global mutex_lock at this point too).
    */
    long i;
    struct stm_priv_segment_info_s *pseg = get_priv_segment(in_segment_num);
    struct list_s *mlst = pseg->modified_old_objects;
    struct list_s *mlstm = pseg->modified_old_objects_markers;
    for (i = list_count(mlst); --i >= 0; ) {
        if (list_item(mlst, i) == (uintptr_t)obj) {
            assert(list_count(mlstm) == 2 * list_count(mlst));
            marker[0] = list_item(mlstm, i * 2 + 0);
            marker[1] = list_item(mlstm, i * 2 + 1);
            goto done;
        }
    }
    marker[0] = 0;
    marker[1] = 0;
 done:
    release_marker_lock(segment_base);
}

static void marker_lookup_other_thread_write_write(uint8_t other_segment_num,
                                                   object_t *obj)
{
    uintptr_t marker[2];
    marker_fetch_obj_write(other_segment_num, obj, marker);

    struct stm_priv_segment_info_s *my_pseg, *other_pseg;
    other_pseg = get_priv_segment(other_segment_num);
    my_pseg = get_priv_segment(STM_SEGMENT->segment_num);
    my_pseg->marker_other[0] = 0;
    marker_expand(marker, other_pseg->pub.segment_base, my_pseg->marker_other);
}

static void marker_lookup_other_thread_inev(uint8_t other_segment_num)
{
    /* same as marker_lookup_other_thread_write_write(), but for
       an inevitable contention instead of a write-write contention */
    struct stm_priv_segment_info_s *my_pseg, *other_pseg;
    assert(_has_mutex());
    other_pseg = get_priv_segment(other_segment_num);
    my_pseg = get_priv_segment(STM_SEGMENT->segment_num);
    marker_expand(other_pseg->marker_inev, other_pseg->pub.segment_base,
                  my_pseg->marker_other);
}

static void marker_lookup_same_thread_write_read(object_t *obj)
{
    uintptr_t marker[2];
    marker_fetch_obj_write(STM_SEGMENT->segment_num, obj, marker);

    struct stm_priv_segment_info_s *my_pseg;
    my_pseg = get_priv_segment(STM_SEGMENT->segment_num);
    my_pseg->marker_self[0] = 0;
    my_pseg->marker_other[0] = 0;
    marker_expand(marker, STM_SEGMENT->segment_base, my_pseg->marker_self);
}

static void marker_fetch_inev(void)
{
    uintptr_t marker[2];
    marker_fetch(STM_SEGMENT->running_thread, marker);
    STM_PSEGMENT->marker_inev[0] = marker[0];
    STM_PSEGMENT->marker_inev[1] = marker[1];
}
