

/* callbacks */
void (*stmcb_light_finalizer)(object_t *);
void (*stmcb_finalizer)(object_t *);


static void init_finalizers(struct finalizers_s *f)
{
    f->objects_with_finalizers = list_create();
    f->count_non_young = 0;
    f->run_finalizers = NULL;
    f->running_next = NULL;
}

static void setup_finalizer(void)
{
    init_finalizers(&g_finalizers);
}

static void teardown_finalizer(void)
{
    if (g_finalizers.run_finalizers != NULL)
        list_free(g_finalizers.run_finalizers);
    list_free(g_finalizers.objects_with_finalizers);
    memset(&g_finalizers, 0, sizeof(g_finalizers));
}

static void _commit_finalizers(void)
{
    /* move finalizer lists to g_finalizers for major collections */
    while (__sync_lock_test_and_set(&g_finalizers.lock, 1) != 0) {
        spin_loop();
    }

    if (STM_PSEGMENT->finalizers->run_finalizers != NULL) {
        /* copy 'STM_PSEGMENT->finalizers->run_finalizers' into
           'g_finalizers.run_finalizers', dropping any initial NULLs
           (finalizers already called) */
        struct list_s *src = STM_PSEGMENT->finalizers->run_finalizers;
        uintptr_t frm = 0;
        if (STM_PSEGMENT->finalizers->running_next != NULL) {
            frm = *STM_PSEGMENT->finalizers->running_next;
            assert(frm <= list_count(src));
            *STM_PSEGMENT->finalizers->running_next = (uintptr_t)-1;
        }
        if (frm < list_count(src)) {
            if (g_finalizers.run_finalizers == NULL)
                g_finalizers.run_finalizers = list_create();
            g_finalizers.run_finalizers = list_extend(
                g_finalizers.run_finalizers,
                src, frm);
        }
        list_free(src);
    }

    /* copy the whole 'STM_PSEGMENT->finalizers->objects_with_finalizers'
       into 'g_finalizers.objects_with_finalizers' */
    g_finalizers.objects_with_finalizers = list_extend(
        g_finalizers.objects_with_finalizers,
        STM_PSEGMENT->finalizers->objects_with_finalizers, 0);
    list_free(STM_PSEGMENT->finalizers->objects_with_finalizers);

    free(STM_PSEGMENT->finalizers);
    STM_PSEGMENT->finalizers = NULL;

    __sync_lock_release(&g_finalizers.lock);
}

static void abort_finalizers(struct stm_priv_segment_info_s *pseg)
{
    /* like _commit_finalizers(), but forget everything from the
       current transaction */
    if (pseg->finalizers != NULL) {
        if (pseg->finalizers->run_finalizers != NULL) {
            if (pseg->finalizers->running_next != NULL) {
                *pseg->finalizers->running_next = (uintptr_t)-1;
            }
            list_free(pseg->finalizers->run_finalizers);
        }
        list_free(pseg->finalizers->objects_with_finalizers);
        free(pseg->finalizers);
        pseg->finalizers = NULL;
    }

    /* call the light finalizers for objects that are about to
       be forgotten from the current transaction */
    char *old_gs_register = STM_SEGMENT->segment_base;
    bool must_fix_gs = old_gs_register != pseg->pub.segment_base;

    struct list_s *lst = pseg->young_objects_with_light_finalizers;
    long i, count = list_count(lst);
    if (lst > 0) {
        for (i = 0; i < count; i++) {
            object_t *obj = (object_t *)list_item(lst, i);
            assert(_is_young(obj));
            if (must_fix_gs) {
                set_gs_register(pseg->pub.segment_base);
                must_fix_gs = false;
            }
            stmcb_light_finalizer(obj);
        }
        list_clear(lst);
    }

    /* also deals with overflow objects: they are at the tail of
       old_objects_with_light_finalizers (this list is kept in order
       and we cannot add any already-committed object) */
    lst = pseg->old_objects_with_light_finalizers;
    count = list_count(lst);
    while (count > 0) {
        object_t *obj = (object_t *)list_item(lst, --count);
        if (!IS_OVERFLOW_OBJ(pseg, obj))
            break;
        lst->count = count;
        if (must_fix_gs) {
            set_gs_register(pseg->pub.segment_base);
            must_fix_gs = false;
        }
        stmcb_light_finalizer(obj);
    }

    if (STM_SEGMENT->segment_base != old_gs_register)
        set_gs_register(old_gs_register);
}


void stm_enable_light_finalizer(object_t *obj)
{
    if (_is_young(obj)) {
        LIST_APPEND(STM_PSEGMENT->young_objects_with_light_finalizers, obj);
    }
    else {
        assert(_is_from_same_transaction(obj));
        LIST_APPEND(STM_PSEGMENT->old_objects_with_light_finalizers, obj);
    }
}

object_t *stm_allocate_with_finalizer(ssize_t size_rounded_up)
{
    object_t *obj = _stm_allocate_external(size_rounded_up);

    if (STM_PSEGMENT->finalizers == NULL) {
        struct finalizers_s *f = malloc(sizeof(struct finalizers_s));
        if (f == NULL)
            stm_fatalerror("out of memory in create_finalizers");   /* XXX */
        init_finalizers(f);
        STM_PSEGMENT->finalizers = f;
    }
    assert(STM_PSEGMENT->finalizers->count_non_young
           <= list_count(STM_PSEGMENT->finalizers->objects_with_finalizers));
    LIST_APPEND(STM_PSEGMENT->finalizers->objects_with_finalizers, obj);
    return obj;
}


/************************************************************/
/*  Light finalizers
*/

static void deal_with_young_objects_with_finalizers(void)
{
    /* for light finalizers: executes finalizers for objs that don't survive
       this minor gc */
    struct list_s *lst = STM_PSEGMENT->young_objects_with_light_finalizers;
    long i, count = list_count(lst);
    for (i = 0; i < count; i++) {
        object_t *obj = (object_t *)list_item(lst, i);
        assert(_is_young(obj));

        object_t *TLPREFIX *pforwarded_array = (object_t *TLPREFIX *)obj;
        if (pforwarded_array[0] != GCWORD_MOVED) {
            /* not moved: the object dies */
            stmcb_light_finalizer(obj);
        }
        else {
            obj = pforwarded_array[1]; /* moved location */
            assert(!_is_young(obj));
            LIST_APPEND(STM_PSEGMENT->old_objects_with_light_finalizers, obj);
        }
    }
    list_clear(lst);
}

static void deal_with_old_objects_with_finalizers(void)
{
    /* for light finalizers */
    int old_gs_register = STM_SEGMENT->segment_num;
    int current_gs_register = old_gs_register;
    long j;
    assert(list_is_empty(get_priv_segment(0)->old_objects_with_light_finalizers));
    for (j = 1; j < NB_SEGMENTS; j++) {
        struct stm_priv_segment_info_s *pseg = get_priv_segment(j);

        struct list_s *lst = pseg->old_objects_with_light_finalizers;
        long i, count = list_count(lst);
        lst->count = 0;
        for (i = 0; i < count; i++) {
            object_t *obj = (object_t *)list_item(lst, i);
            if (!mark_visited_test(obj)) {
                /* not marked: object dies */
                /* we're calling the light finalizer in the same
                   segment as where it was originally registered.  For
                   objects that existed since a long time, it doesn't
                   change anything: any thread should see the same old
                   content (because if it wasn't the case, the object
                   would be in a 'modified_old_objects' list
                   somewhere, and so it wouldn't be dead).  But it's
                   important if the object was created by the same
                   transaction: then only that segment sees valid
                   content.
                */
                if (j != current_gs_register) {
                    set_gs_register(get_segment_base(j));
                    current_gs_register = j;
                }
                stmcb_light_finalizer(obj);
            }
            else {
                /* object survives */
                list_set_item(lst, lst->count++, (uintptr_t)obj);
            }
        }
    }
    if (old_gs_register != current_gs_register)
        set_gs_register(get_segment_base(old_gs_register));
}


/************************************************************/
/*  Algorithm for regular (non-light) finalizers.
    Follows closely pypy/doc/discussion/finalizer-order.rst
    as well as rpython/memory/gc/minimark.py.
*/

static inline int _finalization_state(object_t *obj)
{
    /* Returns the state, "0", 1, 2 or 3, as per finalizer-order.rst.
       One difference is that the official state 0 is returned here
       as a number that is <= 0. */
    struct object_s *realobj = mark_loc(obj);
    if (realobj->stm_flags & GCFLAG_VISITED) {
        if (realobj->stm_flags & GCFLAG_FINALIZATION_ORDERING)
            return 2;
        return 3;
    }

    if (realobj->stm_flags & GCFLAG_FINALIZATION_ORDERING)
        return 1;

    return 0;
}

static void _bump_finalization_state_from_0_to_1(object_t *obj)
{
    assert(_finalization_state(obj) == 0);
    struct object_s *realobj = mark_loc(obj);
    realobj->stm_flags |= GCFLAG_FINALIZATION_ORDERING;
}

static struct list_s *_finalizer_tmpstack;
static struct list_s *_finalizer_emptystack;
static struct list_s *_finalizer_pending;

static inline void _append_to_finalizer_tmpstack(object_t **pobj)
{
    object_t *obj = *pobj;
    if (obj != NULL)
        LIST_APPEND(_finalizer_tmpstack, obj);
}

static inline struct list_s *finalizer_trace(
    struct stm_priv_segment_info_s *pseg, object_t *obj, struct list_s *lst)
{
    char *base;
    if (!is_overflow_obj_safe(pseg, obj))
        base = stm_object_pages;
    else
        base = pseg->pub.segment_base;

    struct object_s *realobj = (struct object_s *)REAL_ADDRESS(base, obj);
    _finalizer_tmpstack = lst;
    stmcb_trace(realobj, &_append_to_finalizer_tmpstack);
    return _finalizer_tmpstack;
}


static void _recursively_bump_finalization_state_from_2_to_3(
    struct stm_priv_segment_info_s *pseg, object_t *obj)
{
    assert(_finalization_state(obj) == 2);
    struct list_s *tmpstack = _finalizer_emptystack;
    assert(list_is_empty(tmpstack));

    while (1) {
        struct object_s *realobj = mark_loc(obj);
        if (realobj->stm_flags & GCFLAG_FINALIZATION_ORDERING) {
            realobj->stm_flags &= ~GCFLAG_FINALIZATION_ORDERING;

            /* trace */
            tmpstack = finalizer_trace(pseg, obj, tmpstack);
        }

        if (list_is_empty(tmpstack))
            break;

        obj = (object_t *)list_pop_item(tmpstack);
    }
    _finalizer_emptystack = tmpstack;
}

static void _recursively_bump_finalization_state_from_1_to_2(
    struct stm_priv_segment_info_s *pseg, object_t *obj)
{
    assert(_finalization_state(obj) == 1);
    /* The call will add GCFLAG_VISITED recursively, thus bump state 1->2 */
    mark_visit_possibly_overflow_object(obj, pseg);
}

static struct list_s *mark_finalize_step1(
    struct stm_priv_segment_info_s *pseg, struct finalizers_s *f)
{
    if (f == NULL)
        return NULL;

    struct list_s *marked = list_create();

    struct list_s *lst = f->objects_with_finalizers;
    long i, count = list_count(lst);
    lst->count = 0;
    f->count_non_young = 0;

    for (i = 0; i < count; i++) {
        object_t *x = (object_t *)list_item(lst, i);

        assert(_finalization_state(x) != 1);
        if (_finalization_state(x) >= 2) {
            list_set_item(lst, lst->count++, (uintptr_t)x);
            continue;
        }
        LIST_APPEND(marked, x);

        struct list_s *pending = _finalizer_pending;
        LIST_APPEND(pending, x);
        while (!list_is_empty(pending)) {
            object_t *y = (object_t *)list_pop_item(pending);
            int state = _finalization_state(y);
            if (state <= 0) {
                _bump_finalization_state_from_0_to_1(y);
                pending = finalizer_trace(pseg, y, pending);
            }
            else if (state == 2) {
                _recursively_bump_finalization_state_from_2_to_3(pseg, y);
            }
        }
        _finalizer_pending = pending;
        assert(_finalization_state(x) == 1);
        _recursively_bump_finalization_state_from_1_to_2(pseg, x);
    }
    return marked;
}

static void mark_finalize_step2(
    struct stm_priv_segment_info_s *pseg, struct finalizers_s *f,
    struct list_s *marked)
{
    if (f == NULL)
        return;

    struct list_s *run_finalizers = f->run_finalizers;

    long i, count = list_count(marked);
    for (i = 0; i < count; i++) {
        object_t *x = (object_t *)list_item(marked, i);

        int state = _finalization_state(x);
        assert(state >= 2);
        if (state == 2) {
            if (run_finalizers == NULL)
                run_finalizers = list_create();
            LIST_APPEND(run_finalizers, x);
            _recursively_bump_finalization_state_from_2_to_3(pseg, x);
        }
        else {
            struct list_s *lst = f->objects_with_finalizers;
            list_set_item(lst, lst->count++, (uintptr_t)x);
        }
    }
    list_free(marked);

    f->run_finalizers = run_finalizers;
}

static void deal_with_objects_with_finalizers(void)
{
    /* for non-light finalizers */
    assert(_has_mutex());
    /* there is one 'objects_with_finalizers' list per segment.
       Objects that die at a major collection running in the same
       transaction as they were created will be put in the
       'run_finalizers' list of that segment.  Objects that survive at
       least one commit move to the global g_objects_with_finalizers,
       and when they die they go to g_run_finalizers.  The former kind
       of dying object must have its finalizer called in the correct
       thread; the latter kind can be called in any thread, through
       any segment, because they all should see the same old content
       anyway.  (If the content was different between segments at this
       point, the object would be in a 'modified_old_objects' list
       somewhere, and so it wouldn't be dead).
    */
    struct list_s *marked_seg[NB_SEGMENTS];
    LIST_CREATE(_finalizer_emptystack);
    LIST_CREATE(_finalizer_pending);

    long j;
    for (j = 1; j < NB_SEGMENTS; j++) {
        struct stm_priv_segment_info_s *pseg = get_priv_segment(j);
        marked_seg[j] = mark_finalize_step1(pseg, pseg->finalizers);
    }
    marked_seg[0] = mark_finalize_step1(get_priv_segment(0), &g_finalizers);

    LIST_FREE(_finalizer_pending);

    for (j = 1; j < NB_SEGMENTS; j++) {
        struct stm_priv_segment_info_s *pseg = get_priv_segment(j);
        mark_finalize_step2(pseg, pseg->finalizers, marked_seg[j]);
    }
    mark_finalize_step2(get_priv_segment(0), &g_finalizers, marked_seg[0]);

    LIST_FREE(_finalizer_emptystack);
}

static void mark_visit_from_finalizer1(
    struct stm_priv_segment_info_s *pseg, struct finalizers_s *f)
{
    if (f != NULL && f->run_finalizers != NULL) {
        LIST_FOREACH_R(f->run_finalizers, object_t * /*item*/,
                       ({
                           mark_visit_possibly_overflow_object(item, pseg);
                       }));
    }
}

static void mark_visit_from_finalizer_pending(void)
{
    long j;
    for (j = 1; j < NB_SEGMENTS; j++) {
        struct stm_priv_segment_info_s *pseg = get_priv_segment(j);
        mark_visit_from_finalizer1(pseg, pseg->finalizers);
    }
    mark_visit_from_finalizer1(get_priv_segment(0), &g_finalizers);
}

static void _execute_finalizers(struct finalizers_s *f)
{
    if (f->run_finalizers == NULL)
        return;   /* nothing to do */

 restart:
    if (f->running_next != NULL)
        return;   /* in a nested invocation of execute_finalizers() */

    uintptr_t next = 0, total = list_count(f->run_finalizers);
    f->running_next = &next;

    while (next < total) {
        object_t *obj = (object_t *)list_item(f->run_finalizers, next);
        list_set_item(f->run_finalizers, next, 0);
        next++;

        stmcb_finalizer(obj);
    }
    if (next == (uintptr_t)-1) {
        /* transaction committed: the whole 'f' was freed */
        return;
    }
    f->running_next = NULL;

    if (f->run_finalizers->count > total) {
        memmove(f->run_finalizers->items,
                f->run_finalizers->items + total,
                (f->run_finalizers->count - total) * sizeof(uintptr_t));
        goto restart;
    }

    LIST_FREE(f->run_finalizers);
}

/* XXX: according to translator.backendopt.finalizer, getfield_gc
        for primitive types is a safe op in light finalizers.
        I don't think that's correct in general (maybe if
        getfield on *dying obj*).
*/

static void _invoke_general_finalizers(stm_thread_local_t *tl, bool own_tx)
{
    /* called between transactions */
    rewind_jmp_buf rjbuf;
    if (own_tx) {
        stm_rewind_jmp_enterframe(tl, &rjbuf);
        _stm_start_transaction(tl);
        /* XXX: become inevitable, bc. otherwise, we would need to keep
           around the original g_finalizers.run_finalizers to restore it
           in case of an abort. */
        _stm_become_inevitable(MSG_INEV_DONT_SLEEP);
        /* did it work? */
        if (STM_PSEGMENT->transaction_state != TS_INEVITABLE) {   /* no */
            /* avoid blocking here, waiting for another INEV transaction.
               If we did that, application code could not proceed (start the
               next transaction) and it will not be obvious from the profile
               why we were WAITing. */
            _stm_commit_transaction();
            stm_rewind_jmp_leaveframe(tl, &rjbuf);
            return;
        }
    } else {
        assert(STM_PSEGMENT->transaction_state == TS_INEVITABLE);
    }

    while (__sync_lock_test_and_set(&g_finalizers.lock, 1) != 0) {
        /* somebody is adding more finalizers (_commit_finalizer()) */
        spin_loop();
    }
    struct finalizers_s copy = g_finalizers;
    assert(copy.running_next == NULL);
    g_finalizers.run_finalizers = NULL;
    /* others may add to g_finalizers again: */
    __sync_lock_release(&g_finalizers.lock);

    if (copy.run_finalizers != NULL) {
        _execute_finalizers(&copy);
    }

    if (own_tx) {
        _stm_commit_transaction();
        stm_rewind_jmp_leaveframe(tl, &rjbuf);
    }

    LIST_FREE(copy.run_finalizers);
}
