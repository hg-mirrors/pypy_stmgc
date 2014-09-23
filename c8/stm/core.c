#ifndef _STM_CORE_H_
# error "must be compiled via stmgc.c"
#endif


/* ############# signal handler ############# */
static void memcpy_to_accessible_pages(
    int dst_segnum, object_t *dst_obj,
    char *src, size_t len, uintptr_t only_page)
{
    /* XXX: optimize */

    char *realobj = REAL_ADDRESS(get_segment_base(dst_segnum), dst_obj);
    char *dst_end = realobj + len;
    uintptr_t loc_addr = (uintptr_t)dst_obj;

    dprintf(("memcpy_to_accessible_pages(%d, %p, %p, %lu, %lu)\n",
             dst_segnum, dst_obj, src, len, only_page));

    while (realobj != dst_end) {
        if (get_page_status_in(dst_segnum, loc_addr / 4096UL) != PAGE_NO_ACCESS
            && (only_page == -1 || only_page == loc_addr / 4096UL)) {
            *realobj = *src;
        }
        realobj++;
        loc_addr++;
        src++;
    }
}


static void copy_bk_objs_in_page_from(int from_segnum, uintptr_t pagenum)
{
    /* looks at all bk copies of objects overlapping page 'pagenum' and
       copies to current segment (never touch PROT_NONE memory). */
    dprintf(("copy_bk_objs_in_page_from(%d, %lu)\n", from_segnum, pagenum));

    acquire_modified_objs_lock(from_segnum);
    abort();
    struct tree_s *tree = NULL; // get_priv_segment(from_segnum)->modified_old_objects;
    wlog_t *item;
    TREE_LOOP_FORWARD(tree, item); {
        object_t *obj = (object_t*)item->addr;
        struct object_s* bk_obj = (struct object_s *)item->val;
        size_t obj_size = stmcb_size_rounded_up(bk_obj);

        if (item->addr < (pagenum + 1) * 4096UL && item->addr + obj_size > pagenum * 4096UL) {
            /* XXX: should actually only write to pagenum, but we validate
               afterwards anyway and abort in case we had modifications there */
            memcpy_to_accessible_pages(STM_SEGMENT->segment_num,
                                       obj, (char*)bk_obj, obj_size, pagenum);
        }
    } TREE_LOOP_END;

    release_modified_objs_lock(from_segnum);
}

static void update_page_revision_from_to(uintptr_t pagenum,
                                         struct stm_commit_log_entry_s *from,
                                         struct stm_commit_log_entry_s *to)
{
    /* walk the commit log and update the page 'pagenum' until we reach
       the same revision as our segment, or we reach the HEAD. */
    assert(all_privatization_locks_acquired());

    volatile struct stm_commit_log_entry_s *cl;
    cl = (volatile struct stm_commit_log_entry_s *)from;

    if (from == to)
        return;

    while ((cl = cl->next)) {
        if (cl == (void *)-1)
            return;

        OPT_ASSERT(cl->segment_num >= 0 && cl->segment_num < NB_SEGMENTS);

        object_t *obj;
        size_t i = 0;
        while ((obj = cl->written[i].object)) {
            abort(); //_update_obj_from(cl->segment_num, obj, pagenum);

            i++;
        };

        /* last fully validated entry */
        if (cl == to)
            return;
    }
}

static void handle_segfault_in_page(uintptr_t pagenum)
{
    /* assumes page 'pagenum' is ACCESS_NONE, privatizes it,
       and validates to newest revision */

    dprintf(("handle_segfault_in_page(%lu), seg %d\n", pagenum, STM_SEGMENT->segment_num));

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
    assert(get_page_status_in(my_segnum, pagenum) == PAGE_PRIVATE);

    /* if there were modifications in the page, revert them: */
    copy_bk_objs_in_page_from(shared_page_holder, pagenum);

    /* if not already newer, update page to our revision */
    update_page_revision_from_to(
        pagenum, get_priv_segment(shared_page_holder)->last_commit_log_entry,
        STM_PSEGMENT->last_commit_log_entry);

    /* in case page is already newer, validate everything now to have a common
       revision for all pages */
    //_stm_validate(NULL, true);
}

static void _signal_handler(int sig, siginfo_t *siginfo, void *context)
{
    int saved_errno = errno;
    char *addr = siginfo->si_addr;
    dprintf(("si_addr: %p\n", addr));
    if (addr == NULL || addr < stm_object_pages ||
        addr >= stm_object_pages+TOTAL_MEMORY) {
        /* actual segfault, unrelated to stmgc */
        fprintf(stderr, "Segmentation fault: accessing %p\n", addr);
        abort();
    }

    int segnum = get_segment_of_linear_address(addr);
    if (segnum != STM_SEGMENT->segment_num) {
        fprintf(stderr, "Segmentation fault: accessing %p (seg %d) from"
                        " seg %d\n", addr, STM_SEGMENT->segment_num, segnum);
        abort();
    }
    dprintf(("-> segment: %d\n", segnum));

    char *seg_base = STM_SEGMENT->segment_base;
    uintptr_t pagenum = ((char*)addr - seg_base) / 4096UL;
    if (pagenum < END_NURSERY_PAGE) {
        fprintf(stderr, "Segmentation fault: accessing %p (seg %d "
                        "page %lu)\n", addr, segnum, pagenum);
        abort();
    }

    handle_segfault_in_page(pagenum);

    errno = saved_errno;
    /* now return and retry */
}

/* ############# commit log ############# */


void _dbg_print_commit_log()
{
    struct stm_commit_log_entry_s *cl = &commit_log_root;

    fprintf(stderr, "commit log root (%p, %d)\n", cl->next, cl->segment_num);
    while ((cl = cl->next)) {
        if (cl == (void *)-1) {
            fprintf(stderr, "  INEVITABLE\n");
            return;
        }
        fprintf(stderr, "  elem (%p, %d)\n", cl->next, cl->segment_num);
        struct stm_undo_s *undo = cl->written;
        struct stm_undo_s *end = undo + cl->written_count;
        for (; undo < end; undo++) {
            fprintf(stderr, "    obj %p, size %d, ofs %lu\n", undo->object,
                    SLICE_SIZE(undo->slice), SLICE_OFFSET(undo->slice));
        }
    }
}

static void reapply_undo_log(struct stm_undo_s *undo)
{
    /* read the object (or object slice) described by 'undo', and
       re-applies it to our current segment.
    */
    dprintf(("_update_obj_from_undo(obj=%p, size=%d, ofs=%lu)\n",
             undo->object, SLICE_SIZE(undo->slice), SLICE_OFFSET(undo->slice)));

    size_t ofs = SLICE_OFFSET(undo->slice);
    size_t size = SLICE_SIZE(undo->slice);
    stm_char *slice_start = ((stm_char *)undo->object) + ofs;
    stm_char *slice_end = slice_start + size;

    uintptr_t page_start = ((uintptr_t)slice_start) / 4096;
    if ((uintptr_t)slice_end <= (page_start + 1) * 4096) {

        /* the object fits inside a single page: fast path */
        if (get_page_status_in(STM_SEGMENT->segment_num, page_start)
            == PAGE_NO_ACCESS) {
            return;   /* ignore the object: it is in a NO_ACCESS page */
        }

        char *src = undo->backup;
        char *dst = REAL_ADDRESS(STM_SEGMENT->segment_base, slice_start);
        memcpy(dst, src, size);
    }
    else {
        abort(); //XXX
    }
}

static void reset_modified_from_backup_copies(int segment_num);  /* forward */

static void _stm_validate(void *free_if_abort)
{
    /* go from last known entry in commit log to the
       most current one and apply all changes done
       by other transactions. Abort if we read one of
       the committed objs. */
    struct stm_commit_log_entry_s *cl = STM_PSEGMENT->last_commit_log_entry;
    struct stm_commit_log_entry_s *next_cl;
    /* Don't check this 'cl'. This entry is already checked */

    bool needs_abort = false;
    while ((next_cl = cl->next) != NULL) {
        if (next_cl == (void *)-1) {
            /* there is an inevitable transaction running */
#if STM_TESTS
            if (free_if_abort != (void *)-1)
                free(free_if_abort);
            stm_abort_transaction();
#endif
            abort();  /* XXX non-busy wait here */
            _stm_collectable_safe_point();
            acquire_all_privatization_locks();
            continue;
        }
        cl = next_cl;

        /*int srcsegnum = cl->segment_num;
          OPT_ASSERT(srcsegnum >= 0 && srcsegnum < NB_SEGMENTS);*/

        struct stm_undo_s *undo = cl->written;
        struct stm_undo_s *end = cl->written + cl->written_count;

        for (; undo < end; undo++) {

            if (_stm_was_read(undo->object)) {
                /* first reset all modified objects from the backup
                   copies as soon as the first conflict is detected;
                   then we will proceed below to update our segment from
                   the old (but unmodified) version to the newer version. */
                if (!needs_abort) {
                    reset_modified_from_backup_copies(STM_SEGMENT->segment_num);
                    needs_abort = true;
                }
            }
            reapply_undo_log(undo);
        }

        /* last fully validated entry */
        STM_PSEGMENT->last_commit_log_entry = cl;
    }

    if (needs_abort) {
        if (free_if_abort != (void *)-1)
            free(free_if_abort);
        stm_abort_transaction();
    }
}

static struct stm_commit_log_entry_s *_create_commit_log_entry(void)
{
    /* puts all modified_old_objects in a new commit log entry */

    // we don't need the privatization lock, as we are only
    // reading from modified_old_objs and nobody but us can change it
    struct list_s *list = STM_PSEGMENT->modified_old_objects;
    OPT_ASSERT((list_count(list) % 3) == 0);
    size_t count = list_count(list) / 3;
    size_t byte_len = sizeof(struct stm_commit_log_entry_s) +
        count * sizeof(struct stm_undo_s);
    struct stm_commit_log_entry_s *result = malloc(byte_len);

    result->next = NULL;
    result->segment_num = STM_SEGMENT->segment_num;
    result->written_count = count;
    memcpy(result->written, list->items, count * sizeof(struct stm_undo_s));
    return result;
}

static void _validate_and_attach(struct stm_commit_log_entry_s *new)
{
    struct stm_commit_log_entry_s *old;

    while (1) {
        _stm_validate(/* free_if_abort =*/ new);

        /* try to attach to commit log: */
        old = STM_PSEGMENT->last_commit_log_entry;
        if (old->next == NULL &&
                __sync_bool_compare_and_swap(&old->next, NULL, new))
            break;   /* success! */
    }
}

static void _validate_and_turn_inevitable(void)
{
    _validate_and_attach((struct stm_commit_log_entry_s *)-1);
}

static void _validate_and_add_to_commit_log(void)
{
    struct stm_commit_log_entry_s *old, *new;

    new = _create_commit_log_entry();
    if (STM_PSEGMENT->transaction_state == TS_INEVITABLE) {
        old = STM_PSEGMENT->last_commit_log_entry;
        OPT_ASSERT(old->next == (void *)-1);

        bool yes = __sync_bool_compare_and_swap(&old->next, (void*)-1, new);
        OPT_ASSERT(yes);
    }
    else {
        _validate_and_attach(new);
    }
}

/* ############# STM ############# */
void stm_validate()
{
    _stm_validate(NULL);
}


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

    dprintf(("write_slowpath(%p): sz=%lu, bk=%p\n", obj, obj_size, bk_obj));
 retry:
    /* privatize pages: */
    acquire_all_privatization_locks();

    uintptr_t page;
    for (page = first_page; page <= end_page; page++) {
        /* check if our page is private or we are the only shared-page holder */
        if (get_page_status_in(my_segnum, page) == PAGE_NO_ACCESS) {
            /* happens if there is a concurrent WB between us making the backup
               and acquiring the locks */
            release_all_privatization_locks();

            volatile char *dummy = REAL_ADDRESS(STM_SEGMENT->segment_base, page * 4096UL);
            *dummy = *dummy;            /* force segfault */

            goto retry;
        }
        assert(get_page_status_in(my_segnum, page) != PAGE_NO_ACCESS);

        if (get_page_status_in(my_segnum, page) == PAGE_PRIVATE)
            continue;

        assert(get_page_status_in(my_segnum, page) == PAGE_SHARED);
        /* make sure all the others are NO_ACCESS
           choosing to make us PRIVATE is harder because then nobody must ever
           update the shared page in stm_validate() except if it is the sole
           reader of it. But then we don't actually know which revision the page is at. */
        int i;
        for (i = 0; i < NB_SEGMENTS; i++) {
            if (i == my_segnum)
                continue;

            if (get_page_status_in(i, page) == PAGE_SHARED) {
                /* xxx: unmap? */
                mprotect((char*)(get_virt_page_of(i, page) * 4096UL), 4096UL, PROT_NONE);
                set_page_status_in(i, page, PAGE_NO_ACCESS);
                dprintf(("NO_ACCESS in seg %d page %lu\n", i, page));
            }
        }
    }
    /* all pages are either private or we were the first to write to a shared
       page and therefore got it as our private one */

    /* remove the WRITE_BARRIER flag */
    obj->stm_flags &= ~GCFLAG_WRITE_BARRIER;

    /* also add it to the GC list for minor collections */
    LIST_APPEND(STM_PSEGMENT->objects_pointing_to_nursery, obj);

    /* phew, now add the obj to the write-set and register the
       backup copy. */
    /* XXX: possibly slow check; try overflow objs again? */
    abort();
    /*if (!tree_contains(STM_PSEGMENT->modified_old_objects, (uintptr_t)obj)) {
        acquire_modified_objs_lock(my_segnum);
        tree_insert(STM_PSEGMENT->modified_old_objects,
                    (uintptr_t)obj, (uintptr_t)bk_obj);
        release_modified_objs_lock(my_segnum);
    }*/
    /* XXX else... what occurs with bk_obj? */

    /* done fiddling with protection and privatization */
    release_all_privatization_locks();
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
             MAP_FIXED | MAP_SHARED | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0) != readmarkers) {
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
    dprintf(("> start_transaction\n"));

    s_mutex_unlock();

    uint8_t old_rv = STM_SEGMENT->transaction_read_version;
    STM_SEGMENT->transaction_read_version = old_rv + 1;
    if (UNLIKELY(old_rv == 0xff)) {
        reset_transaction_read_version();
    }

    assert(list_is_empty(STM_PSEGMENT->modified_old_objects));
    assert(list_is_empty(STM_PSEGMENT->objects_pointing_to_nursery));
    assert(tree_is_cleared(STM_PSEGMENT->young_outside_nursery));
    assert(tree_is_cleared(STM_PSEGMENT->nursery_objects_shadows));
    assert(tree_is_cleared(STM_PSEGMENT->callbacks_on_commit_and_abort[0]));
    assert(tree_is_cleared(STM_PSEGMENT->callbacks_on_commit_and_abort[1]));

    check_nursery_at_transaction_start();

    stm_validate();
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

static void check_all_write_barrier_flags(char *segbase, struct list_s *list)
{
#ifndef NDEBUG
    struct stm_undo_s *undo = (struct stm_undo_s *)list->items;
    struct stm_undo_s *end = (struct stm_undo_s *)(list->items + list->count);
    for (; undo < end; undo++) {
        object_t *obj = undo->object;
        char *dst = REAL_ADDRESS(segbase, obj);
        assert(((struct object_s *)dst)->stm_flags & GCFLAG_WRITE_BARRIER);
    }
#endif
}

void stm_commit_transaction(void)
{
    assert(!_has_mutex());
    assert(STM_PSEGMENT->safe_point == SP_RUNNING);
    assert(STM_PSEGMENT->running_pthread == pthread_self());

    dprintf(("> stm_commit_transaction()\n"));
    minor_collection(1);

    /* minor_collection() above should have set again all WRITE_BARRIER flags.
       Check that again here for the objects that are about to be copied into
       the commit log. */
    check_all_write_barrier_flags(STM_SEGMENT->segment_base,
                                  STM_PSEGMENT->modified_old_objects);

    _validate_and_add_to_commit_log();

    invoke_and_clear_user_callbacks(0);   /* for commit */

    /* XXX do we still need a s_mutex_lock() section here? */
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

static void reset_modified_from_backup_copies(int segment_num)
{
#pragma push_macro("STM_PSEGMENT")
#pragma push_macro("STM_SEGMENT")
#undef STM_PSEGMENT
#undef STM_SEGMENT
    //acquire_modified_objs_lock(segment_num);

    struct stm_priv_segment_info_s *pseg = get_priv_segment(segment_num);
    struct list_s *list = pseg->modified_old_objects;
    struct stm_undo_s *undo = (struct stm_undo_s *)list->items;
    struct stm_undo_s *end = (struct stm_undo_s *)(list->items + list->count);

    for (; undo < end; undo++) {
        object_t *obj = undo->object;
        char *dst = REAL_ADDRESS(pseg->pub.segment_base, obj);

        memcpy(dst + SLICE_OFFSET(undo->slice),
               undo->backup,
               SLICE_SIZE(undo->slice));
        free(undo->backup);
    }

    /* check that all objects have the GCFLAG_WRITE_BARRIER afterwards */
    check_all_write_barrier_flags(pseg->pub.segment_base, list);

    list_clear(list);

    //release_modified_objs_lock(segment_num);

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
