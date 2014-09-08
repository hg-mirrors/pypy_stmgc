#ifndef _STM_CORE_H_
# error "must be compiled via stmgc.c"
#endif

#include <signal.h>


/* ############# signal handler ############# */

static void bring_page_up_to_date(uintptr_t pagenum)
{
    /* Assuming pagenum is not yet private in this segment and
       currently read-protected:

       pagecopy from somewhere readable, then update
       all written objs from that segment */

    assert(!is_shared_log_page(pagenum));

    /* XXX: this may not even be necessary: */
    assert(STM_PSEGMENT->privatization_lock);

    long i;
    int my_segnum = STM_SEGMENT->segment_num;

    assert(!is_readable_log_page_in(my_segnum, pagenum));
    assert(!is_private_log_page_in(my_segnum, pagenum));

    /* privatize and make readable */
    pages_set_protection(my_segnum, pagenum, 1, PROT_READ|PROT_WRITE);
    page_privatize(pagenum);

    /* look for a segment where the page is readable (and
       therefore private): */
    for (i = 0; i < NB_SEGMENTS; i++) {
        if (i == my_segnum)
            continue;
        if (!is_readable_log_page_in(i, pagenum))
            continue;

        acquire_privatization_lock(i);
        /* nobody should be able to change this because we have
           our own privatization lock: */
        assert(is_readable_log_page_in(i, pagenum));

        /* copy the content from there to our segment */
        dprintf(("pagecopy pagenum:%lu, src: %lu, dst:%d\n", pagenum, i, my_segnum));
        pagecopy((char*)(get_virt_page_of(my_segnum, pagenum) * 4096UL),
                 (char*)(get_virt_page_of(i, pagenum) * 4096UL));

        /* get valid state from backup copies of written objs in
           the range of this page: */
        acquire_modified_objs_lock(i);
        struct tree_s *tree = get_priv_segment(i)->modified_old_objects;
        wlog_t *item;
        TREE_LOOP_FORWARD(tree, item); {
            if (item->addr >= pagenum * 4096UL && item->addr < (pagenum + 1) * 4096UL) {
                /* obj is in range. XXX: no page overlapping objs allowed yet */

                object_t *obj = (object_t*)item->addr;
                struct object_s* bk_obj = (struct object_s *)item->val;
                size_t obj_size;

                obj_size = stmcb_size_rounded_up(bk_obj);
                assert(obj_size < 4096); /* XXX */

                memcpy(REAL_ADDRESS(STM_SEGMENT->segment_base, obj),
                       bk_obj, obj_size);

                assert(obj->stm_flags & GCFLAG_WRITE_BARRIER); /* bk_obj never written */
            }
        } TREE_LOOP_END;

        release_modified_objs_lock(i);
        release_privatization_lock(i);

        return;
    }

    abort();                    /* didn't find a page to copy from?? */
}

void _signal_handler(int sig, siginfo_t *siginfo, void *context)
{
    char *addr = siginfo->si_addr;
    dprintf(("si_addr: %p\n", addr));
    if (addr == NULL || addr < stm_object_pages || addr > stm_object_pages+TOTAL_MEMORY) {
        /* actual segfault */
        /* send to GDB (XXX) */
        kill(getpid(), SIGINT);
    }
    /* XXX: should we save 'errno'? */


    int segnum = get_segment_of_linear_address(addr);
    OPT_ASSERT(segnum == STM_SEGMENT->segment_num);
    dprintf(("-> segment: %d\n", segnum));
    char *seg_base = STM_SEGMENT->segment_base;
    uintptr_t pagenum = ((char*)addr - seg_base) / 4096UL;

    acquire_privatization_lock(segnum);
    bring_page_up_to_date(pagenum);
    release_privatization_lock(segnum);

    return;
}

/* ############# commit log ############# */


void _dbg_print_commit_log()
{
    volatile struct stm_commit_log_entry_s *cl;
    cl = (volatile struct stm_commit_log_entry_s *)&commit_log_root;

    fprintf(stderr, "root (%p, %d)\n", cl->next, cl->segment_num);
    while ((cl = cl->next)) {
        if ((uintptr_t)cl == -1) {
            fprintf(stderr, "INEVITABLE\n");
            return;
        }
        size_t i = 0;
        fprintf(stderr, "  elem (%p, %d)\n", cl->next, cl->segment_num);
        object_t *obj;
        while ((obj = cl->written[i])) {
            fprintf(stderr, "-> %p\n", obj);
            i++;
        };
    }
}

static void _update_obj_from(int from_seg, object_t *obj)
{
    /* during validation this looks up the obj in the
       from_seg (backup or normal) and copies the version
       over the current segment's one */
    char *realobj = REAL_ADDRESS(STM_SEGMENT->segment_base, obj);
    size_t obj_size;

    uintptr_t pagenum = (uintptr_t)obj / 4096UL;
    if (!is_private_log_page_in(STM_SEGMENT->segment_num, pagenum)) {
        /* done in signal handler on first access anyway */
        assert(!is_shared_log_page(pagenum));
        assert(!is_readable_log_page_in(STM_SEGMENT->segment_num, pagenum));
        return;
    }

    /* should be readable & private (XXX: maybe not after major GCs) */
    assert(is_readable_log_page_in(from_seg, pagenum));
    assert(is_private_log_page_in(from_seg, pagenum));

    /* look the obj up in the other segment's modified_old_objects to
       get its backup copy: */
    acquire_modified_objs_lock(from_seg);

    struct tree_s *tree = get_priv_segment(from_seg)->modified_old_objects;
    wlog_t *item;
    TREE_FIND(tree, (uintptr_t)obj, item, goto not_found);

    obj_size = stmcb_size_rounded_up((struct object_s*)item->val);
    memcpy(realobj, (char*)item->val, obj_size);
    assert(obj->stm_flags & GCFLAG_WRITE_BARRIER);
    release_modified_objs_lock(from_seg);
    return;

 not_found:
    /* copy from page directly (obj is unmodified) */
    obj_size = stmcb_size_rounded_up(
        (struct object_s*)REAL_ADDRESS(get_segment_base(from_seg), obj));
    memcpy(realobj,
           REAL_ADDRESS(get_segment_base(from_seg), obj),
           obj_size);
    obj->stm_flags |= GCFLAG_WRITE_BARRIER; /* may already be gone */
    release_modified_objs_lock(from_seg);
}

void stm_validate(void *free_if_abort)
{
    /* go from last known entry in commit log to the
       most current one and apply all changes done
       by other transactions. Abort if we read one of
       the committed objs. */
    volatile struct stm_commit_log_entry_s *cl, *prev_cl;
    cl = prev_cl = (volatile struct stm_commit_log_entry_s *)
        STM_PSEGMENT->last_commit_log_entry;

    bool needs_abort = false;
    /* Don't check 'cl'. This entry is already checked */
    while ((cl = cl->next)) {
        if ((uintptr_t)cl == -1) {
            /* there is an inevitable transaction running */
            cl = prev_cl;
            usleep(1);          /* XXX */
            continue;
        }
        prev_cl = cl;

        OPT_ASSERT(cl->segment_num >= 0 && cl->segment_num < NB_SEGMENTS);

        object_t *obj;
        size_t i = 0;
        while ((obj = cl->written[i])) {
            _update_obj_from(cl->segment_num, obj);

            if (!needs_abort && _stm_was_read(obj)) {
                needs_abort = true;
            }

            i++;
        };

        /* last fully validated entry */
        STM_PSEGMENT->last_commit_log_entry = (struct stm_commit_log_entry_s *)cl;
    }

    if (needs_abort) {
        free(free_if_abort);
        stm_abort_transaction();
    }
}

static struct stm_commit_log_entry_s *_create_commit_log_entry()
{
    /* puts all modified_old_objects in a new commit log entry */

    // we don't need the privatization lock, as we are only
    // reading from modified_old_objs and nobody but us can change it
    struct tree_s *tree = STM_PSEGMENT->modified_old_objects;
    size_t count = tree_count(tree);
    size_t byte_len = sizeof(struct stm_commit_log_entry_s) + (count + 1) * sizeof(object_t*);
    struct stm_commit_log_entry_s *result = malloc(byte_len);

    result->next = NULL;
    result->segment_num = STM_SEGMENT->segment_num;

    int i = 0;
    wlog_t *item;
    TREE_LOOP_FORWARD(tree, item); {
        result->written[i] = (object_t*)item->addr;
        i++;
    } TREE_LOOP_END;

    OPT_ASSERT(count == i);
    result->written[count] = NULL;

    return result;
}

static void _validate_and_add_to_commit_log()
{
    struct stm_commit_log_entry_s *new;
    volatile struct stm_commit_log_entry_s **to;

    new = _create_commit_log_entry();
    if (STM_PSEGMENT->transaction_state == TS_INEVITABLE) {
        OPT_ASSERT((uintptr_t)STM_PSEGMENT->last_commit_log_entry->next == -1);

        to = &(STM_PSEGMENT->last_commit_log_entry->next);
        bool yes = __sync_bool_compare_and_swap(to, -1, new);
        OPT_ASSERT(yes);
        return;
    }

    /* regular transaction: */
    do {
        stm_validate(new);

        /* try attaching to commit log: */
        to = &(STM_PSEGMENT->last_commit_log_entry->next);
    } while (!__sync_bool_compare_and_swap(to, NULL, new));
}

static void _validate_and_turn_inevitable()
{
    struct stm_commit_log_entry_s *new;
    volatile struct stm_commit_log_entry_s **to;

    new = (struct stm_commit_log_entry_s*)-1;
    do {
        stm_validate(NULL);

        /* try attaching to commit log: */
        to = &(STM_PSEGMENT->last_commit_log_entry->next);
    } while (!__sync_bool_compare_and_swap(to, NULL, new));
}

/* ############# STM ############# */

void _privatize_and_protect_other_segments(object_t *obj)
{
    /* privatize pages of obj for our segment iff previously
       the pages were fully shared. */
#ifndef NDEBUG
    long l;
    for (l = 0; l < NB_SEGMENTS; l++) {
        assert(get_priv_segment(l)->privatization_lock);
    }
#endif

    uintptr_t first_page = ((uintptr_t)obj) / 4096UL;
    char *realobj;
    size_t obj_size;
    uintptr_t i;
    int my_segnum = STM_SEGMENT->segment_num;

    realobj = REAL_ADDRESS(STM_SEGMENT->segment_base, obj);
    obj_size = stmcb_size_rounded_up((struct object_s *)realobj);
    assert(obj_size < 4096); /* XXX */

    /* privatize pages by read-protecting them in other
       segments and giving us a private copy: */
    assert(is_shared_log_page(first_page));
    /* XXX: change this logic:
       right now, privatization means private in seg0 and private
       in my_segnum */
    for (i = 0; i < NB_SEGMENTS; i++) {
        assert(!is_private_log_page_in(i, first_page));

        if (i != my_segnum && i != 0)
            pages_set_protection(i, first_page, 1, PROT_NONE);
        else                    /* both, seg0 and my_segnum: */
            pages_set_protection(i, first_page, 1, PROT_READ|PROT_WRITE);
    }

    /* remap pages for my_segnum and copy the contents */
    set_page_private_in(0, first_page);
    /* seg0 already up-to-date */
    if (my_segnum != 0) {
        page_privatize(first_page);
        pagecopy((char*)(get_virt_page_of(my_segnum, first_page) * 4096UL),
                 (char*)(get_virt_page_of(0, first_page) * 4096UL));
    }

    assert(is_private_log_page_in(my_segnum, first_page));
    assert(is_readable_log_page_in(my_segnum, first_page));
}

void _stm_write_slowpath(object_t *obj)
{
    assert(_seems_to_be_running_transaction());
    assert(!_is_in_nursery(obj));
    assert(obj->stm_flags & GCFLAG_WRITE_BARRIER);

    int my_segnum = STM_SEGMENT->segment_num;
    uintptr_t first_page = ((uintptr_t)obj) / 4096UL;
    char *realobj;
    size_t obj_size;

    realobj = REAL_ADDRESS(STM_SEGMENT->segment_base, obj);
    obj_size = stmcb_size_rounded_up((struct object_s *)realobj);

    /* add to read set: */
    stm_read(obj);

    /* create backup copy: */
    struct object_s *bk_obj = malloc(obj_size);
    memcpy(bk_obj, realobj, obj_size);

    assert(obj_size < 4096); /* too lazy right now (part of the code is ready) */

    if (is_shared_log_page(first_page)) {
        /* acquire all privatization locks, make private and
           read protect others */
        long i;
        for (i = 0; i < NB_SEGMENTS; i++) {
            acquire_privatization_lock(i);
        }
        if (is_shared_log_page(first_page))
            _privatize_and_protect_other_segments(obj);
        for (i = NB_SEGMENTS-1; i >= 0; i--) {
            release_privatization_lock(i);
        }
    }
    /* page not shared anymore. but we still may have
       only a read protected page ourselves: */

    acquire_privatization_lock(my_segnum);
    if (!is_readable_log_page_in(my_segnum, first_page))
        bring_page_up_to_date(first_page);
    /* page is not PROT_NONE for us */

    /* remove the WRITE_BARRIER flag */
    obj->stm_flags &= ~GCFLAG_WRITE_BARRIER;

    /* done fiddling with protection and privatization */
    release_privatization_lock(my_segnum);

    /* phew, now add the obj to the write-set and register the
       backup copy. */
    acquire_modified_objs_lock(my_segnum);
    tree_insert(STM_PSEGMENT->modified_old_objects,
                (uintptr_t)obj, (uintptr_t)bk_obj);
    release_modified_objs_lock(my_segnum);

    /* also add it to the GC list for minor collections */
    LIST_APPEND(STM_PSEGMENT->objects_pointing_to_nursery, obj);
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


static void _stm_start_transaction(stm_thread_local_t *tl)
{
    assert(!_stm_in_transaction(tl));

  retry:

    if (!acquire_thread_segment(tl))
        goto retry;
    /* GS invalid before this point! */

    assert(STM_PSEGMENT->transaction_state == TS_NONE);
    STM_PSEGMENT->transaction_state = TS_REGULAR;
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

    assert(tree_is_cleared(STM_PSEGMENT->modified_old_objects));
    assert(list_is_empty(STM_PSEGMENT->objects_pointing_to_nursery));
    check_nursery_at_transaction_start();
    stm_validate(NULL);
}

long stm_start_transaction(stm_thread_local_t *tl)
{
    s_mutex_lock();
#ifdef STM_NO_AUTOMATIC_SETJMP
    long repeat_count = 0;    /* test/support.py */
#else
    long repeat_count = stm_rewind_jmp_setjmp(tl);
#endif
    _stm_start_transaction(tl);
    return repeat_count;
}

void stm_start_inevitable_transaction(stm_thread_local_t *tl)
{
    s_mutex_lock();
    _stm_start_transaction(tl);
    _stm_become_inevitable("stm_start_inevitable_transaction");
}

#ifdef STM_NO_AUTOMATIC_SETJMP
void _test_run_abort(stm_thread_local_t *tl) __attribute__((noreturn));
int stm_is_inevitable(void)
{
    switch (STM_PSEGMENT->transaction_state) {
    case TS_REGULAR: return 0;
    case TS_INEVITABLE: return 1;
    default: abort();
    }
}
#endif

/************************************************************/

static void _finish_transaction()
{
    stm_thread_local_t *tl = STM_SEGMENT->running_thread;

    STM_PSEGMENT->transaction_state = TS_NONE;
    list_clear(STM_PSEGMENT->objects_pointing_to_nursery);

    release_thread_segment(tl);
    /* cannot access STM_SEGMENT or STM_PSEGMENT from here ! */
}

void stm_commit_transaction(void)
{
    assert(!_has_mutex());
    assert(STM_PSEGMENT->running_pthread == pthread_self());

    dprintf(("stm_commit_transaction()\n"));
    minor_collection(1);

    _validate_and_add_to_commit_log();

    /* clear WRITE_BARRIER flags, free all backup copies,
       and clear the tree: */
    acquire_modified_objs_lock(STM_SEGMENT->segment_num);

    struct tree_s *tree = STM_PSEGMENT->modified_old_objects;
    wlog_t *item;
    TREE_LOOP_FORWARD(tree, item); {
        object_t *obj = (object_t*)item->addr;
        struct object_s* bk_obj = (struct object_s *)item->val;
        free(bk_obj);
        obj->stm_flags |= GCFLAG_WRITE_BARRIER;
    } TREE_LOOP_END;
    tree_clear(tree);

    release_modified_objs_lock(STM_SEGMENT->segment_num);


    s_mutex_lock();

    assert(STM_SEGMENT->nursery_end == NURSERY_END);
    stm_rewind_jmp_forget(STM_SEGMENT->running_thread);

    /* done */
    _finish_transaction();
    /* cannot access STM_SEGMENT or STM_PSEGMENT from here ! */

    s_mutex_unlock();
}

void reset_modified_from_backup_copies(int segment_num)
{
#pragma push_macro("STM_PSEGMENT")
#pragma push_macro("STM_SEGMENT")
#undef STM_PSEGMENT
#undef STM_SEGMENT
    acquire_modified_objs_lock(segment_num);

    struct stm_priv_segment_info_s *pseg = get_priv_segment(segment_num);
    struct tree_s *tree = pseg->modified_old_objects;
    wlog_t *item;
    TREE_LOOP_FORWARD(tree, item); {
        object_t *obj = (object_t*)item->addr;
        struct object_s* bk_obj = (struct object_s *)item->val;
        size_t obj_size;

        obj_size = stmcb_size_rounded_up(bk_obj);
        assert(obj_size < 4096); /* XXX */

        memcpy(REAL_ADDRESS(pseg->pub.segment_base, obj),
               bk_obj, obj_size);
        assert(obj->stm_flags & GCFLAG_WRITE_BARRIER); /* not written */

        free(bk_obj);
    } TREE_LOOP_END;

    tree_clear(tree);

    release_modified_objs_lock(segment_num);

#pragma pop_macro("STM_SEGMENT")
#pragma pop_macro("STM_PSEGMENT")
}

static void abort_data_structures_from_segment_num(int segment_num)
{
#pragma push_macro("STM_PSEGMENT")
#pragma push_macro("STM_SEGMENT")
#undef STM_PSEGMENT
#undef STM_SEGMENT
    struct stm_priv_segment_info_s *pseg = get_priv_segment(segment_num);

    switch (pseg->transaction_state) {
    case TS_REGULAR:
        break;
    case TS_INEVITABLE:
        stm_fatalerror("abort: transaction_state == TS_INEVITABLE");
    default:
        stm_fatalerror("abort: bad transaction_state == %d",
                       (int)pseg->transaction_state);
    }

    throw_away_nursery(pseg);

    reset_modified_from_backup_copies(segment_num);

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


void _stm_become_inevitable(const char *msg)
{
    s_mutex_lock();

    if (STM_PSEGMENT->transaction_state == TS_REGULAR) {
        dprintf(("become_inevitable: %s\n", msg));

        _validate_and_turn_inevitable();
        STM_PSEGMENT->transaction_state = TS_INEVITABLE;
        stm_rewind_jmp_forget(STM_SEGMENT->running_thread);
    }
    else {
        assert(STM_PSEGMENT->transaction_state == TS_INEVITABLE);
    }

    s_mutex_unlock();
}
