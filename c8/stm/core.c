#ifndef _STM_CORE_H_
# error "must be compiled via stmgc.c"
#endif

#include <signal.h>


/* ############# signal handler ############# */

void _signal_handler(int sig, siginfo_t *siginfo, void *context)
{
    char *addr = siginfo->si_addr;
    dprintf(("si_addr: %p\n", addr));
    if (addr == NULL) { /* actual segfault */
        /* send to GDB */
        kill(getpid(), SIGINT);
    }
    /* XXX: should we save 'errno'? */

    /* make PROT_READWRITE again and validate */
    int segnum = get_segment_of_linear_address(addr);
    OPT_ASSERT(segnum == STM_SEGMENT->segment_num);
    dprintf(("-> segment: %d\n", segnum));
    char *seg_base = STM_SEGMENT->segment_base;
    uintptr_t pagenum = ((char*)addr - seg_base) / 4096UL;

    /* XXX: missing synchronisation: we may change protection, then
       another thread changes it back, then we try to privatize that
       calls page_copy() and traps */
    /* XXX: mprotect is not reentrant and interruptible by signals,
       so it needs additional synchronisation.*/
    mprotect(seg_base + pagenum * 4096UL, 4096, PROT_READ|PROT_WRITE);
    page_privatize(pagenum);

    /* XXX: ... what can go wrong when we abort from inside
       the signal handler? */

    /* make sure we are up to date in this (and all other) pages */
    stm_validate(NULL);
    return;
}

/* ############# commit log ############# */


void _dbg_print_commit_log()
{
    struct stm_commit_log_entry_s *cl = &commit_log_root;

    fprintf(stderr, "root (%p, %d)\n", cl->next, cl->segment_num);
    while ((cl = cl->next)) {
        size_t i = 0;
        fprintf(stderr, "elem (%p, %d)\n", cl->next, cl->segment_num);
        object_t *obj;
        do {
            obj = cl->written[i];
            fprintf(stderr, "-> %p\n", obj);
            i++;
        } while ((obj = cl->written[i]));
    }
}

static void _update_obj_from(int from_seg, object_t *obj)
{
    /* check if its pages are private, only then we need
       to update them. If they are also still read-protected,
       we may trigger the signal handler. This would cause
       it to also enter stm_validate()..... */
}

void stm_validate(void *free_if_abort)
{
    struct stm_commit_log_entry_s *cl = STM_PSEGMENT->last_commit_log_entry;

    /* Don't check 'cl'. This entry is already checked */
    while ((cl = cl->next)) {
        size_t i = 0;
        OPT_ASSERT(cl->segment_num >= 0 && cl->segment_num < NB_SEGMENTS);

        object_t *obj;
        while ((obj = cl->written[i])) {
            /* in case this entry's transaction has not yet discarded
               the backup copies, wait. */
            while (cl->committing)
                spin_loop();

            _update_obj_from(cl->segment_num, obj);

            if (_stm_was_read(obj)) {
                free(free_if_abort);
                stm_abort_transaction();
            }

            i++;
        };

        /* last fully validated entry */
        STM_PSEGMENT->last_commit_log_entry = cl;
    }
}

static struct stm_commit_log_entry_s *_create_commit_log_entry()
{
    struct list_s *lst = STM_PSEGMENT->modified_old_objects;
    size_t count = list_count(lst);
    size_t byte_len = sizeof(struct stm_commit_log_entry_s) + (count + 1) * sizeof(object_t*);
    struct stm_commit_log_entry_s *result = malloc(byte_len);
    int i;

    result->next = NULL;
    result->segment_num = STM_SEGMENT->segment_num;
    for (i = 0; i < count; i++) {
        result->written[i] = (object_t*)list_item(lst, i);
    }
    result->written[count] = NULL;
    result->committing = false;
    spinlock_acquire(result->committing); /* =true */

    return result;
}

static struct stm_commit_log_entry_s *_validate_and_add_to_commit_log()
{
    struct stm_commit_log_entry_s *new, **to;

    new = _create_commit_log_entry();
    fprintf(stderr,"%p\n", new);
    do {
        stm_validate(new);

        to = &(STM_PSEGMENT->last_commit_log_entry->next);
    } while (!__sync_bool_compare_and_swap(to, NULL, new));

    return new;
}

/* ############# STM ############# */

void _stm_write_slowpath(object_t *obj)
{
    assert(_seems_to_be_running_transaction());
    assert(!_is_in_nursery(obj));
    assert(obj->stm_flags & GCFLAG_WRITE_BARRIER);

    stm_read(obj);

    /* XXX: misses synchronisation with other write_barriers
       on same page */
    /* XXX: make backup copy */
    /* XXX: privatize pages if necessary */

    /* make other segments trap if accessing this object */
    uintptr_t first_page = ((uintptr_t)obj) / 4096UL;
    char *realobj;
    size_t obj_size;
    uintptr_t i, end_page;

    realobj = REAL_ADDRESS(STM_SEGMENT->segment_base, obj);
    obj_size = stmcb_size_rounded_up((struct object_s *)realobj);
    end_page = (((uintptr_t)obj) + obj_size - 1) / 4096UL;

    for (i = 0; i < NB_SEGMENTS; i++) {
        if (i == STM_SEGMENT->segment_num)
            continue;
        /* XXX: only do it if not already PROT_NONE */
        char *segment_base = get_segment_base(i);
        mprotect(segment_base + first_page * 4096,
                 (end_page - first_page + 1) * 4096, PROT_NONE);
        dprintf(("prot %lu, len=%lu in seg %lu\n", first_page, (end_page - first_page + 1), i));
    }

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
    STM_PSEGMENT->shadowstack_at_start_of_transaction = tl->shadowstack;

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

    struct stm_commit_log_entry_s* entry = _validate_and_add_to_commit_log();
    /* XXX:discard backup copies */
    spinlock_release(entry->committing);

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
    struct stm_priv_segment_info_s *pseg = get_priv_segment(segment_num);

    throw_away_nursery(pseg);

    /* XXX: reset_modified_from_other_segments(segment_num); */

    stm_thread_local_t *tl = pseg->pub.running_thread;
#ifdef STM_NO_AUTOMATIC_SETJMP
    /* In tests, we don't save and restore the shadowstack correctly.
       Be sure to not change items below shadowstack_at_start_of_transaction.
       There is no such restrictions in non-Python-based tests. */
    assert(tl->shadowstack >= pseg->shadowstack_at_start_of_transaction);
    tl->shadowstack = pseg->shadowstack_at_start_of_transaction;
#else
    /* NB. careful, this function might be called more than once to
       abort a given segment.  Make sure that
       stm_rewind_jmp_restore_shadowstack() is idempotent. */
    /* we need to do this here and not directly in rewind_longjmp() because
       that is called when we already released everything (safe point)
       and a concurrent major GC could mess things up. */
    if (tl->shadowstack != NULL)
        stm_rewind_jmp_restore_shadowstack(tl);
    assert(tl->shadowstack == pseg->shadowstack_at_start_of_transaction);
#endif

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
