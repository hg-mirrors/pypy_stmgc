#ifndef _STM_CORE_H_
# error "must be compiled via stmgc.c"
#endif

/************************************************************/

#define NURSERY_START         (FIRST_NURSERY_PAGE * 4096UL)
#define NURSERY_SIZE          (NB_NURSERY_PAGES * 4096UL)
#define NURSERY_END           (NURSERY_START + NURSERY_SIZE)

static uintptr_t _stm_nursery_start;


/************************************************************/

static void setup_nursery(void)
{
    assert(_STM_FAST_ALLOC <= NURSERY_SIZE);
    _stm_nursery_start = NURSERY_START;

    long i;
    for (i = 0; i < NB_SEGMENTS; i++) {
        get_segment(i)->nursery_current = (stm_char *)NURSERY_START;
        get_segment(i)->nursery_end = NURSERY_END;
    }
}

static inline bool _is_in_nursery(object_t *obj)
{
    assert((uintptr_t)obj >= NURSERY_START);
    return (uintptr_t)obj < NURSERY_END;
}

static inline bool _is_young(object_t *obj)
{
    return (_is_in_nursery(obj) ||
        tree_contains(STM_PSEGMENT->young_outside_nursery, (uintptr_t)obj));
}

long stm_can_move(object_t *obj)
{
    /* 'long' return value to avoid using 'bool' in the public interface */
    return _is_in_nursery(obj);
}


/************************************************************/
static object_t *find_existing_shadow(object_t *obj);
#define GCWORD_MOVED  ((object_t *) -1)

static void minor_trace_if_young(object_t **pobj)
{
    /* takes a normal pointer to a thread-local pointer to an object */
    object_t *obj = *pobj;
    object_t *nobj;
    char *realobj;
    size_t size;

    if (obj == NULL)
        return;
    assert((uintptr_t)obj < NB_PAGES * 4096UL);

    if (_is_in_nursery(obj)) {
        /* If the object was already seen here, its first word was set
           to GCWORD_MOVED.  In that case, the forwarding location, i.e.
           where the object moved to, is stored in the second word in 'obj'. */
        object_t *TLPREFIX *pforwarded_array = (object_t *TLPREFIX *)obj;

        if (obj->stm_flags & GCFLAG_HAS_SHADOW) {
            /* ^^ the single check above detects both already-moved objects
               and objects with HAS_SHADOW.  This is because GCWORD_MOVED
               overrides completely the stm_flags field with 1's bits. */

            if (LIKELY(pforwarded_array[0] == GCWORD_MOVED)) {
                *pobj = pforwarded_array[1];    /* already moved */
                return;
            }
            else {
                /* really has a shadow */
                nobj = find_existing_shadow(obj);
                obj->stm_flags &= ~GCFLAG_HAS_SHADOW;
                realobj = REAL_ADDRESS(STM_SEGMENT->segment_base, obj);
                size = stmcb_size_rounded_up((struct object_s *)realobj);
                goto copy_large_object;
            }
        }

        realobj = REAL_ADDRESS(STM_SEGMENT->segment_base, obj);
        size = stmcb_size_rounded_up((struct object_s *)realobj);

        /* XXX: small objs */
        char *allocated = allocate_outside_nursery_large(size);
        nobj = (object_t *)(allocated - stm_object_pages);

    copy_large_object:;
        char *realnobj = REAL_ADDRESS(STM_SEGMENT->segment_base, nobj);
        memcpy(realnobj, realobj, size);

        pforwarded_array[0] = GCWORD_MOVED;
        pforwarded_array[1] = nobj;
        *pobj = nobj;
    }
    else {
        /* The object was not in the nursery at all */
        if (LIKELY(!tree_contains(STM_PSEGMENT->young_outside_nursery,
                                  (uintptr_t)obj)))
            return;   /* common case: it was an old object, nothing to do */

        /* a young object outside the nursery */
        nobj = obj;
        tree_delete_item(STM_PSEGMENT->young_outside_nursery, (uintptr_t)nobj);
    }

    /* Must trace the object later */
    LIST_APPEND(STM_PSEGMENT->objects_pointing_to_nursery, (uintptr_t)nobj);
}


static void collect_roots_in_nursery(void)
{
    stm_thread_local_t *tl = STM_SEGMENT->running_thread;
    struct stm_shadowentry_s *current = tl->shadowstack;
    struct stm_shadowentry_s *finalbase = tl->shadowstack_base;
    struct stm_shadowentry_s *ssbase;
    ssbase = (struct stm_shadowentry_s *)tl->rjthread.moved_off_ssbase;
    if (ssbase == NULL)
        ssbase = finalbase;
    else
        assert(finalbase <= ssbase && ssbase <= current);

    while (current > ssbase) {
        --current;
        uintptr_t x = (uintptr_t)current->ss;

        if ((x & 3) == 0) {
            /* the stack entry is a regular pointer (possibly NULL) */
            minor_trace_if_young(&current->ss);
        }
        else {
            /* it is an odd-valued marker, ignore */
        }
    }
}


static inline void _collect_now(object_t *obj)
{
    assert(!_is_young(obj));

    dprintf(("_collect_now: %p\n", obj));

    assert(!(obj->stm_flags & GCFLAG_WRITE_BARRIER));

    char *realobj = REAL_ADDRESS(STM_SEGMENT->segment_base, obj);
    stmcb_trace((struct object_s *)realobj, &minor_trace_if_young);

    obj->stm_flags |= GCFLAG_WRITE_BARRIER;
}


static void collect_oldrefs_to_nursery(void)
{
    dprintf(("collect_oldrefs_to_nursery\n"));
    struct list_s *lst = STM_PSEGMENT->objects_pointing_to_nursery;

    while (!list_is_empty(lst)) {
        object_t *obj = (object_t *)list_pop_item(lst);;

        _collect_now(obj);

        /* the list could have moved while appending */
        lst = STM_PSEGMENT->objects_pointing_to_nursery;
    }
}


static size_t throw_away_nursery(struct stm_priv_segment_info_s *pseg)
{
#pragma push_macro("STM_PSEGMENT")
#pragma push_macro("STM_SEGMENT")
#undef STM_PSEGMENT
#undef STM_SEGMENT
    dprintf(("throw_away_nursery\n"));
    /* reset the nursery by zeroing it */
    size_t nursery_used;
    char *realnursery;

    realnursery = REAL_ADDRESS(pseg->pub.segment_base, _stm_nursery_start);
    nursery_used = pseg->pub.nursery_current - (stm_char *)_stm_nursery_start;
    if (nursery_used > NB_NURSERY_PAGES * 4096) {
        /* possible in rare cases when the program artificially advances
           its own nursery_current */
        nursery_used = NB_NURSERY_PAGES * 4096;
    }
    OPT_ASSERT((nursery_used & 7) == 0);
    memset(realnursery, 0, nursery_used);

    /* assert that the rest of the nursery still contains only zeroes */
    assert_memset_zero(realnursery + nursery_used,
                       (NURSERY_END - _stm_nursery_start) - nursery_used);

    pseg->pub.nursery_current = (stm_char *)_stm_nursery_start;

    /* free any object left from 'young_outside_nursery' */
    if (!tree_is_cleared(pseg->young_outside_nursery)) {
        wlog_t *item;

        if (!tree_is_empty(pseg->young_outside_nursery)) {
            /* tree may still be empty even if not cleared */
            TREE_LOOP_FORWARD(pseg->young_outside_nursery, item) {
                object_t *obj = (object_t*)item->addr;
                assert(!_is_in_nursery(obj));

                /* mark slot as unread (it can only have the read marker
                   in this segment) */
                *((char *)(pseg->pub.segment_base + (((uintptr_t)obj) >> 4))) = 0;

                /* XXX: _stm_large_free(stm_object_pages + item->addr); */
            } TREE_LOOP_END;
        }

        tree_clear(pseg->young_outside_nursery);
    }

    tree_clear(pseg->nursery_objects_shadows);

    return nursery_used;
#pragma pop_macro("STM_SEGMENT")
#pragma pop_macro("STM_PSEGMENT")
}

#define MINOR_NOTHING_TO_DO(pseg)                                       \
    ((pseg)->pub.nursery_current == (stm_char *)_stm_nursery_start &&   \
     tree_is_cleared((pseg)->young_outside_nursery))


static void _do_minor_collection(bool commit)
{
    dprintf(("minor_collection commit=%d\n", (int)commit));

    collect_roots_in_nursery();

    collect_oldrefs_to_nursery();

    assert(list_is_empty(STM_PSEGMENT->objects_pointing_to_nursery));

    throw_away_nursery(get_priv_segment(STM_SEGMENT->segment_num));

    assert(MINOR_NOTHING_TO_DO(STM_PSEGMENT));
}

static void minor_collection(bool commit)
{
    assert(!_has_mutex());

    stm_safe_point();

    _do_minor_collection(commit);
}

void stm_collect(long level)
{
    if (level > 0)
        abort();

    minor_collection(/*commit=*/ false);
    /* XXX: major_collection_if_requested(); */
}



/************************************************************/


object_t *_stm_allocate_slowpath(ssize_t size_rounded_up)
{
    /* may collect! */
    STM_SEGMENT->nursery_current -= size_rounded_up;  /* restore correct val */

 restart:
    stm_safe_point();

    OPT_ASSERT(size_rounded_up >= 16);
    OPT_ASSERT((size_rounded_up & 7) == 0);
    OPT_ASSERT(size_rounded_up < _STM_FAST_ALLOC);

    stm_char *p = STM_SEGMENT->nursery_current;
    stm_char *end = p + size_rounded_up;
    if ((uintptr_t)end <= NURSERY_END) {
        STM_SEGMENT->nursery_current = end;
        return (object_t *)p;
    }

    stm_collect(0);
    goto restart;
}

object_t *_stm_allocate_external(ssize_t size_rounded_up)
{
    /* /\* first, force a collection if needed *\/ */
    /* if (is_major_collection_requested()) { */
    /*     /\* use stm_collect() with level 0: if another thread does a major GC */
    /*        in-between, is_major_collection_requested() will become false */
    /*        again, and we'll avoid doing yet another one afterwards. *\/ */
    /*     stm_collect(0); */
    /* } */

    char *result = allocate_outside_nursery_large(size_rounded_up);
    object_t *o = (object_t *)(result - stm_object_pages);

    tree_insert(STM_PSEGMENT->young_outside_nursery, (uintptr_t)o, 0);

    memset(REAL_ADDRESS(STM_SEGMENT->segment_base, o), 0, size_rounded_up);
    return o;
}


#ifdef STM_TESTS
void _stm_set_nursery_free_count(uint64_t free_count)
{
    assert(free_count <= NURSERY_SIZE);
    assert((free_count & 7) == 0);
    _stm_nursery_start = NURSERY_END - free_count;

    long i;
    for (i = 0; i < NB_SEGMENTS; i++) {
        if ((uintptr_t)get_segment(i)->nursery_current < _stm_nursery_start)
            get_segment(i)->nursery_current = (stm_char *)_stm_nursery_start;
    }
}
#endif

static void assert_memset_zero(void *s, size_t n)
{
#ifndef NDEBUG
    size_t i;
# ifndef STM_TESTS
    if (n > 5000) n = 5000;
# endif
    n /= 8;
    for (i = 0; i < n; i++)
        assert(((uint64_t *)s)[i] == 0);
#endif
}

static void check_nursery_at_transaction_start(void)
{
    assert((uintptr_t)STM_SEGMENT->nursery_current == _stm_nursery_start);
    assert_memset_zero(REAL_ADDRESS(STM_SEGMENT->segment_base,
                                    STM_SEGMENT->nursery_current),
                       NURSERY_END - _stm_nursery_start);
}


static object_t *allocate_shadow(object_t *obj)
{
    char *realobj = REAL_ADDRESS(STM_SEGMENT->segment_base, obj);
    size_t size = stmcb_size_rounded_up((struct object_s *)realobj);

    /* always gets outside as a large object for now */
    char *allocated = allocate_outside_nursery_large(size);
    object_t *nobj = (object_t *)(allocated - stm_object_pages);

    /* Initialize the shadow enough to be considered a valid gc object.
       If the original object stays alive at the next minor collection,
       it will anyway be copied over the shadow and overwrite the
       following fields.  But if the object dies, then the shadow will
       stay around and only be freed at the next major collection, at
       which point we want it to look valid (but ready to be freed).

       Here, in the general case, it requires copying the whole object.
       It could be more optimized in special cases like in PyPy, by
       copying only the typeid and (for var-sized objects) the length
       field.  It's probably overkill to add a special stmcb_xxx
       interface just for that.
    */
    char *realnobj = REAL_ADDRESS(STM_SEGMENT->segment_base, nobj);
    memcpy(realnobj, realobj, size);

    obj->stm_flags |= GCFLAG_HAS_SHADOW;

    tree_insert(STM_PSEGMENT->nursery_objects_shadows,
                (uintptr_t)obj, (uintptr_t)nobj);
    return nobj;
}

static object_t *find_existing_shadow(object_t *obj)
{
    wlog_t *item;

    TREE_FIND(STM_PSEGMENT->nursery_objects_shadows,
              (uintptr_t)obj, item, goto not_found);

    /* The answer is the address of the shadow. */
    return (object_t *)item->val;

 not_found:
    stm_fatalerror("GCFLAG_HAS_SHADOW but no shadow found");
}

static object_t *find_shadow(object_t *obj)
{
    /* The object 'obj' is still in the nursery.  Find or allocate a
        "shadow" object, which is where the object will be moved by the
        next minor collection
    */
    if (obj->stm_flags & GCFLAG_HAS_SHADOW)
        return find_existing_shadow(obj);
    else
        return allocate_shadow(obj);
}
