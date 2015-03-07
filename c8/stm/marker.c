#ifndef _STM_CORE_H_
# error "must be compiled via stmgc.c"
#endif


static void marker_fetch(stm_loc_marker_t *out_marker)
{
    /* Fetch the current marker from the 'out_marker->tl's shadow stack,
       and return it in 'out_marker->odd_number' and 'out_marker->object'. */
    stm_thread_local_t *tl = out_marker->tl;
    struct stm_shadowentry_s *current = tl->shadowstack - 1;
    struct stm_shadowentry_s *base = tl->shadowstack_base;

    /* The shadowstack_base contains -1, which is a convenient stopper for
       the loop below but which shouldn't be returned. */
    assert(base->ss == (object_t *)-1);

    while (!(((uintptr_t)current->ss) & 1)) {
        current--;
        assert(current >= base);
    }
    if (current != base) {
        /* found the odd marker */
        out_marker->odd_number = (uintptr_t)current[0].ss;
        out_marker->object = current[1].ss;
    }
    else {
        /* no marker found */
        out_marker->odd_number = 0;
        out_marker->object = NULL;
    }
}

static void marker_fetch_obj_write(object_t *obj, stm_loc_marker_t *out_marker)
{
    /* From 'out_marker->tl', fill in 'out_marker->segment_base' and
       'out_marker->odd_number' and 'out_marker->object' from the
       marker associated with writing the 'obj'.
    */
    assert(_has_mutex());

    long i, num;
    int in_segment_num = out_marker->tl->associated_segment_num;
    assert(in_segment_num >= 1);
    struct stm_priv_segment_info_s *pseg = get_priv_segment(in_segment_num);
    struct list_s *mlst = pseg->modified_old_objects;
    struct list_s *mlstm = pseg->modified_old_objects_markers;
    num = list_count(mlstm) / 2;
    assert(num * 3 <= list_count(mlst));
    for (i = 0; i < num; i++) {
        if (list_item(mlst, i * 3) == (uintptr_t)obj) {
            out_marker->odd_number = list_item(mlstm, i * 2 + 0);
            out_marker->object = (object_t *)list_item(mlstm, i * 2 + 1);
            return;
        }
    }
    out_marker->odd_number = 0;
    out_marker->object = NULL;
}

static void _timing_record_write(void)
{
    stm_loc_marker_t marker;
    marker.tl = STM_SEGMENT->running_thread;
    marker_fetch(&marker);

    long base_count = list_count(STM_PSEGMENT->modified_old_objects) / 3;
    struct list_s *mlstm = STM_PSEGMENT->modified_old_objects_markers;
    while (list_count(mlstm) < 2 * base_count) {
        mlstm = list_append2(mlstm, 0, 0);
    }
    mlstm = list_append2(mlstm, marker.odd_number, (uintptr_t)marker.object);
    STM_PSEGMENT->modified_old_objects_markers = mlstm;
}

static void timing_write_read_contention(object_t *obj)
{
    if (stmcb_timing_event == NULL)
        return;

    /* Collect the older location of the write from the current thread. */
    stm_loc_marker_t marker;
    marker.tl = STM_SEGMENT->running_thread;
    marker.segment_base = STM_SEGMENT->segment_base;
    marker_fetch_obj_write(obj, &marker);

    stmcb_timing_event(marker.tl, STM_CONTENTION_WRITE_READ, &marker);
}


void (*stmcb_timing_event)(stm_thread_local_t *tl, /* the local thread */
                           enum stm_event_e event,
                           stm_loc_marker_t *marker);
