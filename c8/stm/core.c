#ifndef _STM_CORE_H_
# error "must be compiled via stmgc.c"
#endif


/* ############# signal handler ############# */
static void _update_obj_from(int from_seg, object_t *obj);

static void copy_bk_objs_from(int from_segnum, uintptr_t pagenum)
{
    acquire_modified_objs_lock(from_segnum);
    struct tree_s *tree = get_priv_segment(from_segnum)->modified_old_objects;
    wlog_t *item;
    TREE_LOOP_FORWARD(tree, item); {
        if (item->addr >= pagenum * 4096UL && item->addr < (pagenum + 1) * 4096UL) {
            object_t *obj = (object_t*)item->addr;
            struct object_s* bk_obj = (struct object_s *)item->val;
            size_t obj_size;

            obj_size = stmcb_size_rounded_up(bk_obj);

            memcpy_to_accessible_pages(STM_SEGMENT->segment_num,
                                       obj, (char*)bk_obj, obj_size);

            assert(obj->stm_flags & GCFLAG_WRITE_BARRIER); /* bk_obj never written */
        }
    } TREE_LOOP_END;

    release_modified_objs_lock(from_segnum);
}

static void update_page_from_to(
    uintptr_t pagenum, struct stm_commit_log_entry_s *from,
    struct stm_commit_log_entry_s *to)
{
    assert(all_privatization_locks_acquired());

    volatile struct stm_commit_log_entry_s *cl;
    cl = (volatile struct stm_commit_log_entry_s *)from;

    if (from == to)
        return;

    while ((cl = cl->next)) {
        if ((uintptr_t)cl == -1)
            return;

        OPT_ASSERT(cl->segment_num >= 0 && cl->segment_num < NB_SEGMENTS);

        object_t *obj;
        size_t i = 0;
        while ((obj = cl->written[i])) {
            _update_obj_from(cl->segment_num, obj);

            i++;
        };

        /* last fully validated entry */
        if (cl == to)
            return;
    }
}

static void bring_page_up_to_date(uintptr_t pagenum)
{
    /* XXX: bad, but no deadlocks: */
    acquire_all_privatization_locks();

    long i;
    int my_segnum = STM_SEGMENT->segment_num;

    assert(get_page_status_in(my_segnum, pagenum) == PAGE_NO_ACCESS);

    /* find who has the PAGE_SHARED */
    int shared_page_holder = -1;
    int shared_ref_count = 0;
    for (i = 0; i < NB_SEGMENTS; i++) {
        if (i == my_segnum)
            continue;
        if (get_page_status_in(i, pagenum) == PAGE_SHARED) {
            shared_page_holder = i;
            shared_ref_count++;
        }
    }
    assert(shared_page_holder != -1);

    /* XXX: for now, we don't try to get the single shared page. We simply
       regard it as private for its holder. */
    /* this assert should be true for now... */
    assert(shared_ref_count == 1);

    /* make our page private */
    page_privatize_in(STM_SEGMENT->segment_num, pagenum);

    /* if there were modifications in the page, revert them: */
    copy_bk_objs_from(shared_page_holder, pagenum);

    /* if not already newer, update page to our revision */
    update_page_from_to(
        pagenum, get_priv_segment(shared_page_holder)->last_commit_log_entry,
        STM_PSEGMENT->last_commit_log_entry);

    /* in case page is already newer, validate everything now to have a common
       revision for all pages */
    stm_validate(NULL);

    release_all_privatization_locks();
}

static void _signal_handler(int sig, siginfo_t *siginfo, void *context)
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

    bring_page_up_to_date(pagenum);

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
    size_t obj_size;

    /* look the obj up in the other segment's modified_old_objects to
       get its backup copy: */
    acquire_modified_objs_lock(from_seg);

    wlog_t *item;
    struct tree_s *tree = get_priv_segment(from_seg)->modified_old_objects;
    TREE_FIND(tree, (uintptr_t)obj, item, goto not_found);

    obj_size = stmcb_size_rounded_up((struct object_s*)item->val);

    memcpy_to_accessible_pages(STM_SEGMENT->segment_num, obj,
                               (char*)item->val, obj_size);

    assert(obj->stm_flags & GCFLAG_WRITE_BARRIER);
    release_modified_objs_lock(from_seg);
    return;

 not_found:
    /* copy from page directly (obj is unmodified) */
    obj_size = stmcb_size_rounded_up(
        (struct object_s*)REAL_ADDRESS(get_segment_base(from_seg), obj));

    memcpy_to_accessible_pages(STM_SEGMENT->segment_num, obj,
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
    if (STM_PSEGMENT->transaction_state == TS_INEVITABLE) {
        assert((uintptr_t)STM_PSEGMENT->last_commit_log_entry->next == -1);
        return;
    }
    assert(all_privatization_locks_acquired());

    volatile struct stm_commit_log_entry_s *cl, *prev_cl;
    cl = prev_cl = (volatile struct stm_commit_log_entry_s *)
        STM_PSEGMENT->last_commit_log_entry;

    bool needs_abort = false;
    /* Don't check 'cl'. This entry is already checked */
    while ((cl = cl->next)) {
        if ((uintptr_t)cl == -1) {
            /* there is an inevitable transaction running */
#if STM_TESTS
            free(free_if_abort);
            stm_abort_transaction();
#endif
            cl = prev_cl;
            _stm_collectable_safe_point();
            continue;
        }
        prev_cl = cl;

        OPT_ASSERT(cl->segment_num >= 0 && cl->segment_num < NB_SEGMENTS);

        object_t *obj;
        size_t i = 0;
        while ((obj = cl->written[i])) {
            _update_obj_from(cl->segment_num, obj);

            if (_stm_was_read(obj)) {
                needs_abort = true;

                /* if we wrote this obj, we need to free its backup and
                   remove it from modified_old_objects because
                   we would otherwise overwrite the updated obj on abort */
                acquire_modified_objs_lock(STM_SEGMENT->segment_num);
                wlog_t *item;
                struct tree_s *tree = STM_PSEGMENT->modified_old_objects;
                TREE_FIND(tree, (uintptr_t)obj, item, goto not_found);

                free((void*)item->val);
                TREE_FIND_DELETE(tree, item);

            not_found:
                /* nothing todo */
                release_modified_objs_lock(STM_SEGMENT->segment_num);
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
        bool yes = __sync_bool_compare_and_swap(to, (void*)-1, new);
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

void _stm_write_slowpath(object_t *obj)
{
    assert(_seems_to_be_running_transaction());
    assert(!_is_in_nursery(obj));
    assert(obj->stm_flags & GCFLAG_WRITE_BARRIER);

    int my_segnum = STM_SEGMENT->segment_num;
    uintptr_t end_page, first_page = ((uintptr_t)obj) / 4096UL;
    char *realobj;
    size_t obj_size;

    realobj = REAL_ADDRESS(STM_SEGMENT->segment_base, obj);
    obj_size = stmcb_size_rounded_up((struct object_s *)realobj);
    /* get the last page containing data from the object */
    end_page = (((uintptr_t)obj) + obj_size - 1) / 4096UL;

    /* add to read set: */
    stm_read(obj);

    /* create backup copy (this may cause several page faults XXX): */
    struct object_s *bk_obj = malloc(obj_size);
    memcpy(bk_obj, realobj, obj_size);

    /* privatize pages: */
    acquire_all_privatization_locks();

    uintptr_t page;
    for (page = first_page; page <= end_page; page++) {
        /* check if our page is private or we are the only shared-page holder */
        assert(get_page_status_in(my_segnum, page) != PAGE_NO_ACCESS);

        if (get_page_status_in(my_segnum, page) == PAGE_PRIVATE)
            continue;

        /* make sure all the others are NO_ACCESS
           choosing to make us PRIVATE is harder because then nobody must ever
           update the shared page in stm_validate() except if it is the sole
           reader of it. But then we don't actually know which revision the page is at. */
        long i;
        for (i = 0; i < NB_SEGMENTS; i++) {
            if (i == my_segnum)
                continue;

            if (get_page_status_in(i, page) == PAGE_SHARED) {
                /* xxx: unmap? */
                mprotect((char*)(get_virt_page_of(i, page) * 4096UL), 4096UL, PROT_NONE);
                set_page_status_in(i, page, PAGE_NO_ACCESS);
            }
        }
    }
    /* all pages are either private or we were the first to write to a shared
       page and therefore got it as our private one */

    /* remove the WRITE_BARRIER flag */
    obj->stm_flags &= ~GCFLAG_WRITE_BARRIER;

    /* also add it to the GC list for minor collections */
    LIST_APPEND(STM_PSEGMENT->objects_pointing_to_nursery, obj);

    /* done fiddling with protection and privatization */
    release_all_privatization_locks();

    /* phew, now add the obj to the write-set and register the
       backup copy. */
    /* XXX: possibly slow check; try overflow objs again? */
    if (!tree_contains(STM_PSEGMENT->modified_old_objects, (uintptr_t)obj)) {
        acquire_modified_objs_lock(my_segnum);
        tree_insert(STM_PSEGMENT->modified_old_objects,
                    (uintptr_t)obj, (uintptr_t)bk_obj);
        release_modified_objs_lock(my_segnum);
    }

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

    assert(STM_PSEGMENT->safe_point == SP_NO_TRANSACTION);
    assert(STM_PSEGMENT->transaction_state == TS_NONE);
    STM_PSEGMENT->transaction_state = TS_REGULAR;
    STM_PSEGMENT->safe_point = SP_RUNNING;
#ifndef NDEBUG
    STM_PSEGMENT->running_pthread = pthread_self();
#endif
    STM_PSEGMENT->shadowstack_at_start_of_transaction = tl->shadowstack;

    enter_safe_point_if_requested();
    dprintf(("start_transaction\n"));

    s_mutex_unlock();

    uint8_t old_rv = STM_SEGMENT->transaction_read_version;
    STM_SEGMENT->transaction_read_version = old_rv + 1;
    if (UNLIKELY(old_rv == 0xff)) {
        reset_transaction_read_version();
    }

    assert(tree_is_cleared(STM_PSEGMENT->modified_old_objects));
    assert(list_is_empty(STM_PSEGMENT->objects_pointing_to_nursery));
    assert(tree_is_cleared(STM_PSEGMENT->young_outside_nursery));
    assert(tree_is_cleared(STM_PSEGMENT->nursery_objects_shadows));
    assert(tree_is_cleared(STM_PSEGMENT->callbacks_on_commit_and_abort[0]));
    assert(tree_is_cleared(STM_PSEGMENT->callbacks_on_commit_and_abort[1]));

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

    STM_PSEGMENT->safe_point = SP_NO_TRANSACTION;
    STM_PSEGMENT->transaction_state = TS_NONE;
    list_clear(STM_PSEGMENT->objects_pointing_to_nursery);

    release_thread_segment(tl);
    /* cannot access STM_SEGMENT or STM_PSEGMENT from here ! */
}

void stm_commit_transaction(void)
{
    assert(!_has_mutex());
    assert(STM_PSEGMENT->safe_point == SP_RUNNING);
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

    invoke_and_clear_user_callbacks(0);   /* for commit */

    s_mutex_lock();
    enter_safe_point_if_requested();
    assert(STM_SEGMENT->nursery_end == NURSERY_END);

    stm_rewind_jmp_forget(STM_SEGMENT->running_thread);

    if (globally_unique_transaction && STM_PSEGMENT->transaction_state == TS_INEVITABLE) {
        committed_globally_unique_transaction();
    }

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

    long bytes_in_nursery = throw_away_nursery(pseg);

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
tl->last_abort__bytes_in_nursery = bytes_in_nursery;

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

    if (tl->mem_clear_on_abort)
        memset(tl->mem_clear_on_abort, 0, tl->mem_bytes_to_clear_on_abort);

    invoke_and_clear_user_callbacks(1);   /* for abort */

    if (is_abort(STM_SEGMENT->nursery_end)) {
        /* done aborting */
        STM_SEGMENT->nursery_end = pause_signalled ? NSE_SIGPAUSE
                                                   : NURSERY_END;
    }

    _finish_transaction();
    /* cannot access STM_SEGMENT or STM_PSEGMENT from here ! */

    return tl;
}

static void abort_with_mutex(void)
{
    stm_thread_local_t *tl = abort_with_mutex_no_longjmp();
    s_mutex_unlock();

    usleep(1);

#ifdef STM_NO_AUTOMATIC_SETJMP
    _test_run_abort(tl);
#else
    s_mutex_lock();
    stm_rewind_jmp_longjmp(tl);
#endif
}



#ifdef STM_NO_AUTOMATIC_SETJMP
void _test_run_abort(stm_thread_local_t *tl) __attribute__((noreturn));
#endif

void stm_abort_transaction(void)
{
    s_mutex_lock();
    abort_with_mutex();
}


void _stm_become_inevitable(const char *msg)
{
    if (STM_PSEGMENT->transaction_state == TS_REGULAR) {
        dprintf(("become_inevitable: %s\n", msg));
        _stm_collectable_safe_point();

        _validate_and_turn_inevitable();
        STM_PSEGMENT->transaction_state = TS_INEVITABLE;
        stm_rewind_jmp_forget(STM_SEGMENT->running_thread);
        invoke_and_clear_user_callbacks(0);   /* for commit */
    }
    else {
        assert(STM_PSEGMENT->transaction_state == TS_INEVITABLE);
    }
}

void stm_become_globally_unique_transaction(stm_thread_local_t *tl,
                                            const char *msg)
{
    stm_become_inevitable(tl, msg);   /* may still abort */

    s_mutex_lock();
    synchronize_all_threads(STOP_OTHERS_AND_BECOME_GLOBALLY_UNIQUE);
    s_mutex_unlock();
}
