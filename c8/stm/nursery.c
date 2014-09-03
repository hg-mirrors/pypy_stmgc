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

long stm_can_move(object_t *obj)
{
    /* 'long' return value to avoid using 'bool' in the public interface */
    return _is_in_nursery(obj);
}


/************************************************************/

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


    return nursery_used;
#pragma pop_macro("STM_SEGMENT")
#pragma pop_macro("STM_PSEGMENT")
}


static void _do_minor_collection(bool commit)
{
    dprintf(("minor_collection commit=%d\n", (int)commit));

    assert(list_is_empty(STM_PSEGMENT->objects_pointing_to_nursery));

    throw_away_nursery(get_priv_segment(STM_SEGMENT->segment_num));
}

static void minor_collection(bool commit)
{
    assert(!_has_mutex());

    _do_minor_collection(commit);
}


/************************************************************/


object_t *_stm_allocate_slowpath(ssize_t size_rounded_up)
{
    /* may collect! */
    STM_SEGMENT->nursery_current -= size_rounded_up;  /* restore correct val */

 restart:

    OPT_ASSERT(size_rounded_up >= 16);
    OPT_ASSERT((size_rounded_up & 7) == 0);

    stm_char *p = STM_SEGMENT->nursery_current;
    stm_char *end = p + size_rounded_up;
    if ((uintptr_t)end <= NURSERY_END) {
        STM_SEGMENT->nursery_current = end;
        return (object_t *)p;
    }

    abort();//stm_collect(0);
    goto restart;
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
