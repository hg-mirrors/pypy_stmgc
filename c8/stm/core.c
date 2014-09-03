#ifndef _STM_CORE_H_
# error "must be compiled via stmgc.c"
#endif


void _stm_write_slowpath(object_t *obj)
{
    assert(_seems_to_be_running_transaction());
    assert(!_is_in_nursery(obj));
    assert(obj->stm_flags & GCFLAG_WRITE_BARRIER);

    stm_read(obj);

    LIST_APPEND(STM_PSEGMENT->modified_old_objects, obj);

    LIST_APPEND(STM_PSEGMENT->objects_pointing_to_nursery, obj);
    obj->stm_flags &= ~GCFLAG_WRITE_BARRIER;
}

static void reset_transaction_read_version(void)
{
    /* force-reset all read markers to 0 */

    char *readmarkers = REAL_ADDRESS(STM_SEGMENT->segment_base,
                                     FIRST_READMARKER_PAGE * 4096UL);
    dprintf(("reset_transaction_read_version: %p %ld\n", readmarkers,
             (long)(NB_READMARKER_PAGES * 4096UL)));
    if (mmap(readmarkers, NB_READMARKER_PAGES * 4096UL,
             PROT_READ | PROT_WRITE,
             MAP_FIXED | MAP_PAGES_FLAGS, -1, 0) != readmarkers) {
        /* fall-back */
#if STM_TESTS
        stm_fatalerror("reset_transaction_read_version: %m");
#endif
        memset(readmarkers, 0, NB_READMARKER_PAGES * 4096UL);
    }
    STM_SEGMENT->transaction_read_version = 1;
}


static void _stm_start_transaction(stm_thread_local_t *tl, bool inevitable)
{
    assert(!_stm_in_transaction(tl));

  retry:

    if (!acquire_thread_segment(tl))
        goto retry;
    /* GS invalid before this point! */

#ifndef NDEBUG
    STM_PSEGMENT->running_pthread = pthread_self();
#endif

    dprintf(("start_transaction\n"));

    s_mutex_unlock();

    uint8_t old_rv = STM_SEGMENT->transaction_read_version;
    STM_SEGMENT->transaction_read_version = old_rv + 1;
    if (UNLIKELY(old_rv == 0xff)) {
        reset_transaction_read_version();
    }

    assert(list_is_empty(STM_PSEGMENT->modified_old_objects));
    assert(list_is_empty(STM_PSEGMENT->objects_pointing_to_nursery));
    check_nursery_at_transaction_start();
}

long stm_start_transaction(stm_thread_local_t *tl)
{
    s_mutex_lock();
#ifdef STM_NO_AUTOMATIC_SETJMP
    long repeat_count = 0;    /* test/support.py */
#else
    long repeat_count = stm_rewind_jmp_setjmp(tl);
#endif
    _stm_start_transaction(tl, false);
    return repeat_count;
}


/************************************************************/

static void _page_wise_synchronize_object_now(object_t *obj)
{
    uintptr_t start = (uintptr_t)obj;
    uintptr_t first_page = start / 4096UL;

    char *realobj = REAL_ADDRESS(STM_SEGMENT->segment_base, obj);
    ssize_t obj_size = stmcb_size_rounded_up((struct object_s *)realobj);
    assert(obj_size >= 16);
    uintptr_t end = start + obj_size;
    uintptr_t last_page = (end - 1) / 4096UL;
    long i, myself = STM_SEGMENT->segment_num;

    for (; first_page <= last_page; first_page++) {

        uintptr_t copy_size;
        if (first_page == last_page) {
            /* this is the final fragment */
            copy_size = end - start;
        }
        else {
            /* this is a non-final fragment, going up to the
               page's end */
            copy_size = 4096 - (start & 4095);
        }
        /* double-check that the result fits in one page */
        assert(copy_size > 0);
        assert(copy_size + (start & 4095) <= 4096);

        char *dst, *src;
        src = REAL_ADDRESS(STM_SEGMENT->segment_base, start);
        for (i = 0; i < NB_SEGMENTS; i++) {
            if (i == myself)
                continue;

            dst = REAL_ADDRESS(get_segment_base(i), start);
            if (is_private_page(i, first_page)) {
                /* The page is a private page.  We need to diffuse this
                   fragment of object from the shared page to this private
                   page. */
                if (copy_size == 4096)
                    pagecopy(dst, src);
                else
                    memcpy(dst, src, copy_size);
            }
            else {
                assert(!memcmp(dst, src, copy_size));  /* same page */
            }
        }

        start = (start + 4096) & ~4095;
    }
}

void _push_obj_to_other_segments(object_t *obj)
{
    acquire_privatization_lock();
    _page_wise_synchronize_object_now(obj);
    release_privatization_lock();
}

static void _finish_transaction()
{
    stm_thread_local_t *tl = STM_SEGMENT->running_thread;

    list_clear(STM_PSEGMENT->objects_pointing_to_nursery);

    release_thread_segment(tl);
    /* cannot access STM_SEGMENT or STM_PSEGMENT from here ! */
}

void stm_commit_transaction(void)
{
    assert(!_has_mutex());
    assert(STM_PSEGMENT->running_pthread == pthread_self());

    minor_collection(1);

    s_mutex_lock();

    assert(STM_SEGMENT->nursery_end == NURSERY_END);
    stm_rewind_jmp_forget(STM_SEGMENT->running_thread);


    /* done */
    _finish_transaction();
    /* cannot access STM_SEGMENT or STM_PSEGMENT from here ! */

    s_mutex_unlock();
}


static void abort_data_structures_from_segment_num(int segment_num)
{
#pragma push_macro("STM_PSEGMENT")
#pragma push_macro("STM_SEGMENT")
#undef STM_PSEGMENT
#undef STM_SEGMENT
    /* struct stm_priv_segment_info_s *pseg = get_priv_segment(segment_num); */

    /* throw_away_nursery(pseg); */

    /* reset_modified_from_other_segments(segment_num); */

#pragma pop_macro("STM_SEGMENT")
#pragma pop_macro("STM_PSEGMENT")
}


static stm_thread_local_t *abort_with_mutex_no_longjmp(void)
{
    assert(_has_mutex());
    dprintf(("~~~ ABORT\n"));

    assert(STM_PSEGMENT->running_pthread == pthread_self());

    abort_data_structures_from_segment_num(STM_SEGMENT->segment_num);

    stm_thread_local_t *tl = STM_SEGMENT->running_thread;

    _finish_transaction();
    /* cannot access STM_SEGMENT or STM_PSEGMENT from here ! */

    return tl;
}


#ifdef STM_NO_AUTOMATIC_SETJMP
void _test_run_abort(stm_thread_local_t *tl) __attribute__((noreturn));
#endif

void stm_abort_transaction(void)
{
    s_mutex_lock();
    stm_thread_local_t *tl = abort_with_mutex_no_longjmp();
    s_mutex_unlock();

#ifdef STM_NO_AUTOMATIC_SETJMP
    _test_run_abort(tl);
#else
    s_mutex_lock();
    stm_rewind_jmp_longjmp(tl);
#endif
}
