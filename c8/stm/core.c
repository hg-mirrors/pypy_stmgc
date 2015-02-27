#ifndef _STM_CORE_H_
# error "must be compiled via stmgc.c"
#endif

/* *** MISC *** */
static void free_bk(struct stm_undo_s *undo)
{
    free(undo->backup);
    assert(undo->backup = (char*)-88);
    increment_total_allocated(-SLICE_SIZE(undo->slice));
}

static struct stm_commit_log_entry_s *malloc_cle(long entries)
{
    size_t byte_len = sizeof(struct stm_commit_log_entry_s) +
        entries * sizeof(struct stm_undo_s);
    struct stm_commit_log_entry_s *result = malloc(byte_len);
    increment_total_allocated(byte_len);
    return result;
}

static void free_cle(struct stm_commit_log_entry_s *e)
{
    size_t byte_len = sizeof(struct stm_commit_log_entry_s) +
        e->written_count * sizeof(struct stm_undo_s);
    increment_total_allocated(-byte_len);
    free(e);
}
/* *** MISC *** */


/* General helper: copies objects into our own segment, from some
   source described by a range of 'struct stm_undo_s'.  Maybe later
   we could specialize this function to avoid the checks in the
   inner loop.
*/
static void import_objects(
        int from_segnum,            /* or -1: from undo->backup,
                                       or -2: from undo->backup if not modified */
        uintptr_t pagenum,          /* or -1: "all accessible" */
        struct stm_undo_s *undo,
        struct stm_undo_s *end)
{
    char *src_segment_base = (from_segnum >= 0 ? get_segment_base(from_segnum)
                                               : NULL);

    assert(IMPLY(from_segnum >= 0, get_priv_segment(from_segnum)->modification_lock));
    assert(STM_PSEGMENT->modification_lock);

    DEBUG_EXPECT_SEGFAULT(false);
    for (; undo < end; undo++) {
        object_t *obj = undo->object;
        stm_char *oslice = ((stm_char *)obj) + SLICE_OFFSET(undo->slice);
        uintptr_t current_page_num = ((uintptr_t)oslice) / 4096;

        if (pagenum == -1) {
            if (get_page_status_in(STM_SEGMENT->segment_num,
                                   current_page_num) == PAGE_NO_ACCESS)
                continue;
        }
        else {
            if (current_page_num != pagenum)
                continue;
        }

        if (from_segnum == -2 && _stm_was_read(obj) && (obj->stm_flags & GCFLAG_WB_EXECUTED)) {
            /* called from stm_validate():
                > if not was_read(), we certainly didn't modify
                > if not WB_EXECUTED, we may have read from the obj in a different page but
                  did not modify it (should not occur right now, but future proof!)
               only the WB_EXECUTED alone is not enough, since we may have imported from a
               segment's private page (which had the flag set) */
            assert(IMPLY(_stm_was_read(obj), (obj->stm_flags & GCFLAG_WB_EXECUTED))); /* for now */
            continue;           /* only copy unmodified */
        }

        /* XXX: if the next assert is always true, we should never get a segfault
           in this function at all. So the DEBUG_EXPECT_SEGFAULT is correct. */
        assert((get_page_status_in(STM_SEGMENT->segment_num,
                                   current_page_num) != PAGE_NO_ACCESS));

        /* dprintf(("import slice seg=%d obj=%p off=%lu sz=%d pg=%lu\n", */
        /*          from_segnum, obj, SLICE_OFFSET(undo->slice), */
        /*          SLICE_SIZE(undo->slice), current_page_num)); */
        char *src, *dst;
        if (src_segment_base != NULL)
            src = REAL_ADDRESS(src_segment_base, oslice);
        else
            src = undo->backup;
        dst = REAL_ADDRESS(STM_SEGMENT->segment_base, oslice);
        memcpy(dst, src, SLICE_SIZE(undo->slice));

        if (src_segment_base == NULL && SLICE_OFFSET(undo->slice) == 0) {
            /* check that restored obj doesn't have WB_EXECUTED */
            assert(!(obj->stm_flags & GCFLAG_WB_EXECUTED));
        }
    }
    DEBUG_EXPECT_SEGFAULT(true);
}


/* ############# signal handler ############# */

static void copy_bk_objs_in_page_from(int from_segnum, uintptr_t pagenum,
                                      bool only_if_not_modified)
{
    /* looks at all bk copies of objects overlapping page 'pagenum' and
       copies the part in 'pagenum' back to the current segment */
    dprintf(("copy_bk_objs_in_page_from(%d, %ld, %d)\n",
             from_segnum, (long)pagenum, only_if_not_modified));

    struct list_s *list = get_priv_segment(from_segnum)->modified_old_objects;
    struct stm_undo_s *undo = (struct stm_undo_s *)list->items;
    struct stm_undo_s *end = (struct stm_undo_s *)(list->items + list->count);

    import_objects(only_if_not_modified ? -2 : -1,
                   pagenum, undo, end);
}

static void go_to_the_past(uintptr_t pagenum,
                           struct stm_commit_log_entry_s *from,
                           struct stm_commit_log_entry_s *to)
{
    assert(STM_PSEGMENT->modification_lock);
    assert(from->rev_num >= to->rev_num);
    /* walk BACKWARDS the commit log and update the page 'pagenum',
       initially at revision 'from', until we reach the revision 'to'. */

    /* XXXXXXX Recursive algo for now, fix this! */
    if (from != to) {
        struct stm_commit_log_entry_s *cl = to->next;
        go_to_the_past(pagenum, from, cl);

        struct stm_undo_s *undo = cl->written;
        struct stm_undo_s *end = cl->written + cl->written_count;

        import_objects(-1, pagenum, undo, end);
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

    /* find who has the most recent revision of our page */
    int copy_from_segnum = -1;
    uint64_t most_recent_rev = 0;
    for (i = 1; i < NB_SEGMENTS; i++) {
        if (i == my_segnum)
            continue;

        struct stm_commit_log_entry_s *log_entry;
        log_entry = get_priv_segment(i)->last_commit_log_entry;
        if (get_page_status_in(i, pagenum) != PAGE_NO_ACCESS
            && (copy_from_segnum == -1 || log_entry->rev_num > most_recent_rev)) {
            copy_from_segnum = i;
            most_recent_rev = log_entry->rev_num;
        }
    }
    OPT_ASSERT(copy_from_segnum != my_segnum);

    /* make our page write-ready */
    page_mark_accessible(my_segnum, pagenum);

    /* account for this page now: XXX */
    /* increment_total_allocated(4096); */

    if (copy_from_segnum == -1) {
        /* this page is only accessible in the sharing segment seg0 so far (new
           allocation). We can thus simply mark it accessible here. */
        pagecopy(get_virtual_page(my_segnum, pagenum),
                 get_virtual_page(0, pagenum));
        release_all_privatization_locks();
        return;
    }

    /* before copying anything, acquire modification locks from our and
       the other segment */
    uint64_t to_lock = (1UL << copy_from_segnum)| (1UL << my_segnum);
    acquire_modification_lock_set(to_lock);
    pagecopy(get_virtual_page(my_segnum, pagenum),
             get_virtual_page(copy_from_segnum, pagenum));

    /* if there were modifications in the page, revert them. */
    copy_bk_objs_in_page_from(copy_from_segnum, pagenum, false);

    /* we need to go from 'src_version' to 'target_version'.  This
       might need a walk into the past. */
    struct stm_commit_log_entry_s *src_version, *target_version;
    src_version = get_priv_segment(copy_from_segnum)->last_commit_log_entry;
    target_version = STM_PSEGMENT->last_commit_log_entry;


    dprintf(("handle_segfault_in_page: rev %lu to rev %lu\n",
             src_version->rev_num, target_version->rev_num));
    /* adapt revision of page to our revision:
       if our rev is higher than the page we copy from, everything
       is fine as we never read/modified the page anyway
     */
    if (src_version->rev_num > target_version->rev_num)
        go_to_the_past(pagenum, src_version, target_version);

    release_modification_lock_set(to_lock);
    release_all_privatization_locks();
}

static void _signal_handler(int sig, siginfo_t *siginfo, void *context)
{
    assert(_stm_segfault_expected > 0);

    int saved_errno = errno;
    char *addr = siginfo->si_addr;
    dprintf(("si_addr: %p\n", addr));
    if (addr == NULL || addr < stm_object_pages ||
        addr >= stm_object_pages+TOTAL_MEMORY) {
        /* actual segfault, unrelated to stmgc */
        fprintf(stderr, "Segmentation fault: accessing %p\n", addr);
        raise(SIGINT);
    }

    int segnum = get_segment_of_linear_address(addr);
    OPT_ASSERT(segnum != 0);
    if (segnum != STM_SEGMENT->segment_num) {
        fprintf(stderr, "Segmentation fault: accessing %p (seg %d) from"
                " seg %d\n", addr, segnum, STM_SEGMENT->segment_num);
        raise(SIGINT);
    }
    dprintf(("-> segment: %d\n", segnum));

    char *seg_base = STM_SEGMENT->segment_base;
    uintptr_t pagenum = ((char*)addr - seg_base) / 4096UL;
    if (pagenum < END_NURSERY_PAGE) {
        fprintf(stderr, "Segmentation fault: accessing %p (seg %d "
                        "page %lu)\n", addr, segnum, pagenum);
        raise(SIGINT);
    }

    DEBUG_EXPECT_SEGFAULT(false);
    handle_segfault_in_page(pagenum);
    DEBUG_EXPECT_SEGFAULT(true);

    errno = saved_errno;
    /* now return and retry */
}

/* ############# commit log ############# */


void _dbg_print_commit_log()
{
    struct stm_commit_log_entry_s *cl = &commit_log_root;

    fprintf(stderr, "commit log:\n");
    while (cl) {
        fprintf(stderr, "  entry at %p: seg %d, rev %lu\n", cl, cl->segment_num, cl->rev_num);
        struct stm_undo_s *undo = cl->written;
        struct stm_undo_s *end = undo + cl->written_count;
        for (; undo < end; undo++) {
            fprintf(stderr, "    obj %p, size %d, ofs %lu: ", undo->object,
                    SLICE_SIZE(undo->slice), SLICE_OFFSET(undo->slice));
            /* long i; */
            /* for (i=0; i<SLICE_SIZE(undo->slice); i += 8) */
            /*     fprintf(stderr, " 0x%016lx", *(long *)(undo->backup + i)); */
            fprintf(stderr, "\n");
        }

        cl = cl->next;
        if (cl == INEV_RUNNING) {
            fprintf(stderr, "  INEVITABLE\n");
            return;
        }
    }
}

static void reset_modified_from_backup_copies(int segment_num);  /* forward */

static bool _stm_validate()
{
    /* returns true if we reached a valid state, or false if
       we need to abort now */
    dprintf(("_stm_validate() at cl=%p, rev=%lu\n", STM_PSEGMENT->last_commit_log_entry,
             STM_PSEGMENT->last_commit_log_entry->rev_num));
    /* go from last known entry in commit log to the
       most current one and apply all changes done
       by other transactions. Abort if we have read one of
       the committed objs. */
    struct stm_commit_log_entry_s *first_cl = STM_PSEGMENT->last_commit_log_entry;
    struct stm_commit_log_entry_s *next_cl, *last_cl, *cl;
    int my_segnum = STM_SEGMENT->segment_num;
    /* Don't check this 'cl'. This entry is already checked */

    if (STM_PSEGMENT->transaction_state == TS_INEVITABLE) {
        //assert(first_cl->next == INEV_RUNNING);
        /* the above assert may fail when running a major collection
           while the commit of the inevitable transaction is in progress
           and the element is already attached */
        return true;
    }

    bool needs_abort = false;

    while(1) {
        /* retry IF: */
        /* if at the time of "HERE" (s.b.) there happen to be
           more commits (and bk copies) then it could be that
           copy_bk_objs_in_page_from (s.b.) reads a bk copy that
           is itself more recent than last_cl. This is fixed
           by re-validating. */
        first_cl = STM_PSEGMENT->last_commit_log_entry;
        if (first_cl->next == NULL)
            break;

        if (first_cl->next == INEV_RUNNING) {
            /* need to reach safe point if an INEV transaction
               is waiting for us, otherwise deadlock */
            break;
        }

        /* Find the set of segments we need to copy from and lock them: */
        uint64_t segments_to_lock = 1UL << my_segnum;
        cl = first_cl;
        while ((next_cl = cl->next) != NULL) {
            if (next_cl == INEV_RUNNING) {
                /* only validate entries up to INEV */
                break;
            }
            assert(next_cl->rev_num > cl->rev_num);
            cl = next_cl;

            if (cl->written_count) {
                segments_to_lock |= (1UL << cl->segment_num);
            }
        }
        last_cl = cl;

        /* HERE */

        acquire_privatization_lock(STM_SEGMENT->segment_num);
        acquire_modification_lock_set(segments_to_lock);


        /* import objects from first_cl to last_cl: */
        if (first_cl != last_cl) {
            uint64_t segment_really_copied_from = 0UL;

            cl = first_cl;
            while ((cl = cl->next) != NULL) {
                if (!needs_abort) {
                    struct stm_undo_s *undo = cl->written;
                    struct stm_undo_s *end = cl->written + cl->written_count;
                    for (; undo < end; undo++) {
                        if (_stm_was_read(undo->object)) {
                            /* first reset all modified objects from the backup
                               copies as soon as the first conflict is detected;
                               then we will proceed below to update our segment from
                               the old (but unmodified) version to the newer version.
                            */
                            reset_modified_from_backup_copies(my_segnum);
                            needs_abort = true;

                            dprintf(("_stm_validate() failed for obj %p\n", undo->object));
                            break;
                        }
                    }
                }

                if (cl->written_count) {
                    struct stm_undo_s *undo = cl->written;
                    struct stm_undo_s *end = cl->written + cl->written_count;

                    segment_really_copied_from |= (1UL << cl->segment_num);

                    import_objects(cl->segment_num, -1, undo, end);

                    /* here we can actually have our own modified version, so
                       make sure to only copy things that are not modified in our
                       segment... (if we do not abort) */
                    copy_bk_objs_in_page_from
                        (cl->segment_num, -1,     /* any page */
                         !needs_abort);  /* if we abort, we still want to copy everything */
                }

                dprintf(("_stm_validate() to cl=%p, rev=%lu\n", cl, cl->rev_num));
                /* last fully validated entry */
                STM_PSEGMENT->last_commit_log_entry = cl;
                if (cl == last_cl)
                    break;
            }
            assert(cl == last_cl);

            /* XXX: this optimization fails in test_basic.py, bug3 */
            /* OPT_ASSERT(segment_really_copied_from < (1 << NB_SEGMENTS)); */
            /* int segnum; */
            /* for (segnum = 1; segnum < NB_SEGMENTS; segnum++) { */
            /*     if (segment_really_copied_from & (1UL << segnum)) { */
            /*         /\* here we can actually have our own modified version, so */
            /*            make sure to only copy things that are not modified in our */
            /*            segment... (if we do not abort) *\/ */
            /*         copy_bk_objs_in_page_from( */
            /*             segnum, -1,     /\* any page *\/ */
            /*             !needs_abort);  /\* if we abort, we still want to copy everything *\/ */
            /*     } */
            /* } */
        }

        /* done with modifications */
        release_modification_lock_set(segments_to_lock);
        release_privatization_lock(STM_SEGMENT->segment_num);
    }

    return !needs_abort;
}


static struct stm_commit_log_entry_s *_create_commit_log_entry(void)
{
    /* puts all modified_old_objects in a new commit log entry */

    // we don't need the privatization lock, as we are only
    // reading from modified_old_objs and nobody but us can change it
    struct list_s *list = STM_PSEGMENT->modified_old_objects;
    OPT_ASSERT((list_count(list) % 3) == 0);
    size_t count = list_count(list) / 3;
    struct stm_commit_log_entry_s *result = malloc_cle(count);

    result->next = NULL;
    result->segment_num = STM_SEGMENT->segment_num;
    result->rev_num = -1;       /* invalid */
    result->written_count = count;
    memcpy(result->written, list->items, count * sizeof(struct stm_undo_s));
    return result;
}


static void reset_cards_from_modified_objects(void);
static void reset_wb_executed_flags(void);
static void readd_wb_executed_flags(void);
static void check_all_write_barrier_flags(char *segbase, struct list_s *list);

static void _validate_and_attach(struct stm_commit_log_entry_s *new)
{
    struct stm_commit_log_entry_s *old;

    OPT_ASSERT(new != NULL);
    /* we are attaching a real CL entry: */
    bool is_commit = new != INEV_RUNNING;

    while (1) {
        if (!_stm_validate()) {
            if (new != INEV_RUNNING)
                free_cle((struct stm_commit_log_entry_s*)new);
            stm_abort_transaction();
        }

#if STM_TESTS
        if (STM_PSEGMENT->transaction_state != TS_INEVITABLE
            && STM_PSEGMENT->last_commit_log_entry->next == INEV_RUNNING) {
            /* abort for tests... */
            stm_abort_transaction();
        }
#endif

        if (is_commit) {
            /* we must not remove the WB_EXECUTED flags before validation as
               it is part of a condition in import_objects() called by
               copy_bk_objs_in_page_from to not overwrite our modifications.
               So we do it here: */
            reset_wb_executed_flags();
            check_all_write_barrier_flags(STM_SEGMENT->segment_base,
                                          STM_PSEGMENT->modified_old_objects);

            /* need to remove the entries in modified_old_objects "at the same
               time" as the attach to commit log. Otherwise, another thread may
               see the new CL entry, import it, look for backup copies in this
               segment and find the old backup copies! */
            acquire_modification_lock(STM_SEGMENT->segment_num);
        }

        /* try to attach to commit log: */
        old = STM_PSEGMENT->last_commit_log_entry;
        if (old->next == NULL) {
            if (new != INEV_RUNNING) /* INEVITABLE */
                new->rev_num = old->rev_num + 1;

            if (__sync_bool_compare_and_swap(&old->next, NULL, new))
                break;   /* success! */
        } else if (old->next == INEV_RUNNING) {
            /* we failed because there is an INEV transaction running */
            usleep(10);
        }

        if (is_commit) {
            release_modification_lock(STM_SEGMENT->segment_num);
            /* XXX: unfortunately, if we failed to attach our CL entry,
               we have to re-add the WB_EXECUTED flags before we try to
               validate again because of said condition (s.a) */
            readd_wb_executed_flags();
        }

        dprintf(("_validate_and_attach(%p) failed, enter safepoint\n", new));

        /* check for requested safe point. otherwise an INEV transaction
           may try to commit but cannot because of the busy-loop here. */
        /* minor gc is fine here because we did one immediately before, so
           there are no young objs anyway. major gc is fine because the
           modified_old_objects list is still populated with the same
           cl-entry objs */
        /* XXXXXXXX: memory leak if we happen to do a major gc, we get aborted
           in major_do_validation_and_minor_collections, and don't free 'new' */
        _stm_collectable_safe_point();
    }

    if (is_commit) {
        /* compare with _validate_and_add_to_commit_log */
        STM_PSEGMENT->transaction_state = TS_NONE;
        STM_PSEGMENT->safe_point = SP_NO_TRANSACTION;

        reset_cards_from_modified_objects();

        list_clear(STM_PSEGMENT->modified_old_objects);
        STM_PSEGMENT->last_commit_log_entry = new;
        release_modification_lock(STM_SEGMENT->segment_num);
    }
}

static void _validate_and_turn_inevitable(void)
{
    _validate_and_attach((struct stm_commit_log_entry_s *)INEV_RUNNING);
}

static void _validate_and_add_to_commit_log(void)
{
    struct stm_commit_log_entry_s *old, *new;

    new = _create_commit_log_entry();
    if (STM_PSEGMENT->transaction_state == TS_INEVITABLE) {
        old = STM_PSEGMENT->last_commit_log_entry;
        new->rev_num = old->rev_num + 1;
        OPT_ASSERT(old->next == INEV_RUNNING);

        /* WB_EXECUTED must be removed before we attach */
        reset_wb_executed_flags();
        check_all_write_barrier_flags(STM_SEGMENT->segment_base,
                                      STM_PSEGMENT->modified_old_objects);

        reset_cards_from_modified_objects();

        /* compare with _validate_and_attach: */
        STM_PSEGMENT->transaction_state = TS_NONE;
        STM_PSEGMENT->safe_point = SP_NO_TRANSACTION;
        list_clear(STM_PSEGMENT->modified_old_objects);
        STM_PSEGMENT->last_commit_log_entry = new;

        /* do it: */
        bool yes = __sync_bool_compare_and_swap(&old->next, INEV_RUNNING, new);
        OPT_ASSERT(yes);
    }
    else {
        _validate_and_attach(new);
    }
}

/* ############# STM ############# */
void stm_validate()
{
    if (!_stm_validate())
        stm_abort_transaction();
}


bool obj_should_use_cards(char *seg_base, object_t *obj)
{
    if (is_small_uniform(obj))
        return false;

    struct object_s *realobj = (struct object_s *)
        REAL_ADDRESS(seg_base, obj);
    long supports = stmcb_obj_supports_cards(realobj);
    if (!supports)
        return false;

    /* check also if it makes sense: */
    size_t size = stmcb_size_rounded_up(realobj);
    return (size >= _STM_MIN_CARD_OBJ_SIZE);
}

__attribute__((always_inline))
static void write_gc_only_path(object_t *obj, bool mark_card)
{
    assert(obj->stm_flags & GCFLAG_WRITE_BARRIER);
    assert(obj->stm_flags & GCFLAG_WB_EXECUTED);
    dprintf(("write_slowpath-fast(%p)\n", obj));

    if (!mark_card) {
        /* The basic case, with no card marking.  We append the object
           into 'objects_pointing_to_nursery', and remove the flag so
           that the write_slowpath will not be called again until the
           next minor collection. */
        if (obj->stm_flags & GCFLAG_CARDS_SET) {
            /* if we clear this flag, we also need to clear the cards.
               bk_slices are not needed as this is a new object */
            /* XXX: add_missing_bk_slices_and_"clear"_cards */
            _reset_object_cards(get_priv_segment(STM_SEGMENT->segment_num),
                                obj, CARD_CLEAR, false, false);
        }
        obj->stm_flags &= ~(GCFLAG_WRITE_BARRIER | GCFLAG_CARDS_SET);
        LIST_APPEND(STM_PSEGMENT->objects_pointing_to_nursery, obj);
    } else {
        /* Card marking.  Don't remove GCFLAG_WRITE_BARRIER because we
           need to come back to _stm_write_slowpath_card() for every
           card to mark.  Add GCFLAG_CARDS_SET. */
        assert(!(obj->stm_flags & GCFLAG_CARDS_SET));
        obj->stm_flags |= GCFLAG_CARDS_SET;
        LIST_APPEND(STM_PSEGMENT->old_objects_with_cards_set, obj);
    }
}


__attribute__((always_inline))
static void write_slowpath_common(object_t *obj, bool mark_card)
{
    assert(_seems_to_be_running_transaction());
    assert(!_is_in_nursery(obj));
    assert(obj->stm_flags & GCFLAG_WRITE_BARRIER);

    if (obj->stm_flags & GCFLAG_WB_EXECUTED
|| isoverflow) {
        /* already executed WB once in this transaction. do GC
           part again: */
        write_gc_only_path(obj, mark_card);
        return;
    }

    char *realobj;
    size_t obj_size;
    int my_segnum = STM_SEGMENT->segment_num;
    uintptr_t end_page, first_page = ((uintptr_t)obj) / 4096UL;

    realobj = REAL_ADDRESS(STM_SEGMENT->segment_base, obj);
    obj_size = stmcb_size_rounded_up((struct object_s *)realobj);
    /* get the last page containing data from the object */
    if (LIKELY(is_small_uniform(obj))) {
        end_page = first_page;
    } else {
        end_page = (((uintptr_t)obj) + obj_size - 1) / 4096UL;
    }

    /* add to read set: */
    stm_read(obj);

    assert(!(obj->stm_flags & GCFLAG_WB_EXECUTED));
    dprintf(("write_slowpath(%p): sz=%lu\n", obj, obj_size));

 retry:
    /* privatize pages: */
    /* XXX don't always acquire all locks... */
    acquire_all_privatization_locks();

    uintptr_t page;
    for (page = first_page; page <= end_page; page++) {
        if (get_page_status_in(my_segnum, page) == PAGE_NO_ACCESS) {
            /* XXX: slow? */
            release_all_privatization_locks();

            volatile char *dummy = REAL_ADDRESS(STM_SEGMENT->segment_base, page * 4096UL);
            *dummy;            /* force segfault */

            goto retry;
        }
    }
    /* all pages are private to us and we hold the privatization_locks so
       we are allowed to modify them */

    /* phew, now add the obj to the write-set and register the
       backup copy. */
    /* XXX: we should not be here at all fiddling with page status
       if 'obj' is merely an overflow object.  FIX ME, likely by copying
       the overflow number logic from c7. */

    DEBUG_EXPECT_SEGFAULT(false);

    acquire_modification_lock(STM_SEGMENT->segment_num);
    uintptr_t slice_sz;
    uintptr_t in_page_offset = (uintptr_t)obj % 4096UL;
    uintptr_t remaining_obj_sz = obj_size;
    for (page = first_page; page <= end_page; page++) {
        /* XXX Maybe also use mprotect() again to mark pages of the object as read-only, and
           only stick it into modified_old_objects page-by-page?  Maybe it's
           possible to do card-marking that way, too. */
        OPT_ASSERT(remaining_obj_sz);

        slice_sz = remaining_obj_sz;
        if (in_page_offset + slice_sz > 4096UL) {
            /* not over page boundaries */
            slice_sz = 4096UL - in_page_offset;
        }

        size_t slice_off = obj_size - remaining_obj_sz;

        /* make backup slice: */
        char *bk_slice = malloc(slice_sz);
        increment_total_allocated(slice_sz);
        memcpy(bk_slice, realobj + slice_off, slice_sz);

        /* !! follows layout of "struct stm_undo_s" !! */
        STM_PSEGMENT->modified_old_objects = list_append3(
            STM_PSEGMENT->modified_old_objects,
            (uintptr_t)obj,     /* obj */
            (uintptr_t)bk_slice,  /* bk_addr */
            NEW_SLICE(slice_off, slice_sz));

        remaining_obj_sz -= slice_sz;
        in_page_offset = (in_page_offset + slice_sz) % 4096UL; /* mostly 0 */
    }
    OPT_ASSERT(remaining_obj_sz == 0);

    if (!mark_card) {
        /* also add it to the GC list for minor collections */
        LIST_APPEND(STM_PSEGMENT->objects_pointing_to_nursery, obj);

        if (obj->stm_flags & GCFLAG_CARDS_SET) {
            /* if we clear this flag, we have to tell sync_old_objs that
               everything needs to be synced */
            /* if we clear this flag, we have to tell later write barriers
               that we already did all backup slices: */
            /* XXX: do_other_bk_slices_and_"clear"_cards */
            _reset_object_cards(get_priv_segment(STM_SEGMENT->segment_num),
                                obj, STM_SEGMENT->transaction_read_version,
                                true, false); /* mark all */
        }

        /* remove the WRITE_BARRIER flag and add WB_EXECUTED */
        obj->stm_flags &= ~(GCFLAG_WRITE_BARRIER | GCFLAG_CARDS_SET);
        obj->stm_flags |= GCFLAG_WB_EXECUTED;
    } else {
        /* don't remove WRITE_BARRIER, but add CARDS_SET */
        obj->stm_flags |= (GCFLAG_CARDS_SET | GCFLAG_WB_EXECUTED);
        /* XXXXXXXXXXXX maybe not set WB_EXECUTED and make CARDS_SET
           mean the same thing where necessary */
        LIST_APPEND(STM_PSEGMENT->old_objects_with_cards_set, obj);
    }

    DEBUG_EXPECT_SEGFAULT(true);

    release_modification_lock(STM_SEGMENT->segment_num);
    /* done fiddling with protection and privatization */
    release_all_privatization_locks();
}


char _stm_write_slowpath_card_extra(object_t *obj)
{
    /* the PyPy JIT calls this function directly if it finds that an
       array doesn't have the GCFLAG_CARDS_SET */
    bool mark_card = obj_should_use_cards(STM_SEGMENT->segment_base, obj);
    write_slowpath_common(obj, mark_card);
    return mark_card;
}

long _stm_write_slowpath_card_extra_base(void)
{
    /* XXX can go away? */
    /* for the PyPy JIT: _stm_write_slowpath_card_extra_base[obj >> 4]
       is the byte that must be set to CARD_MARKED.  The logic below
       does the same, but more explicitly. */
    return 0;
}

void _stm_write_slowpath_card(object_t *obj, uintptr_t index)
{
    dprintf_test(("write_slowpath_card(%p, %lu)\n",
                  obj, index));

    /* If CARDS_SET is not set so far, issue a normal write barrier.
       If the object is large enough, ask it to set up the object for
       card marking instead. */
    if (!(obj->stm_flags & GCFLAG_CARDS_SET)) {
        char mark_card = _stm_write_slowpath_card_extra(obj);
        if (!mark_card)
            return;
    }

    dprintf_test(("write_slowpath_card %p -> index:%lu\n",
                  obj, index));

    /* We reach this point if we have to mark the card. */
    assert(obj->stm_flags & GCFLAG_WRITE_BARRIER);
    assert(obj->stm_flags & GCFLAG_CARDS_SET);
    assert(!is_small_uniform(obj)); /* not supported/tested */

#ifndef NDEBUG
    struct object_s *realobj = (struct object_s *)
        REAL_ADDRESS(STM_SEGMENT->segment_base, obj);
    size_t size = stmcb_size_rounded_up(realobj);
    /* we need at least one read marker in addition to the STM-reserved object
       write-lock */
    assert(size >= 32);
    /* the 'index' must be in range(length-of-obj), but we don't have
       a direct way to know the length.  We know that it is smaller
       than the size in bytes. */
    assert(index < size);
#endif

    /* Write into the card's lock.  This is used by the next minor
       collection to know what parts of the big object may have changed.
       We already own the object here or it is an overflow obj. */
    struct stm_read_marker_s *cards = get_read_marker(STM_SEGMENT->segment_base,
                                                      (uintptr_t)obj);
    cards[get_index_to_card_index(index)].rm = CARD_MARKED;

    dprintf(("mark %p index %lu, card:%lu with %d\n",
             obj, index, get_index_to_card_index(index), CARD_MARKED));
}

void _stm_write_slowpath(object_t *obj) {
    write_slowpath_common(obj,  /* mark_card */ false);
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
             MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0) != readmarkers) {
        /* fall-back */
#if STM_TESTS
        stm_fatalerror("reset_transaction_read_version: %m");
#endif
        memset(readmarkers, 0, NB_READMARKER_PAGES * 4096UL);
    }
    STM_SEGMENT->transaction_read_version = 2;
    assert(STM_SEGMENT->transaction_read_version > _STM_CARD_MARKED);
}

static void reset_cards_from_modified_objects(void)
{
    struct list_s *list = STM_PSEGMENT->modified_old_objects;
    struct stm_undo_s *undo = (struct stm_undo_s *)list->items;
    struct stm_undo_s *end = (struct stm_undo_s *)(list->items + list->count);

    for (; undo < end; undo++) {
        object_t *obj = undo->object;
        if (obj_should_use_cards(STM_SEGMENT->segment_base, obj))
            _reset_object_cards(get_priv_segment(STM_SEGMENT->segment_num),
                                obj, CARD_CLEAR, false);
    }
}

static void reset_wb_executed_flags(void)
{
    dprintf(("reset_wb_executed_flags()\n"));
    struct list_s *list = STM_PSEGMENT->modified_old_objects;
    struct stm_undo_s *undo = (struct stm_undo_s *)list->items;
    struct stm_undo_s *end = (struct stm_undo_s *)(list->items + list->count);

    for (; undo < end; undo++) {
        object_t *obj = undo->object;
        obj->stm_flags &= ~GCFLAG_WB_EXECUTED;
    }
}

static void readd_wb_executed_flags(void)
{
    dprintf(("readd_wb_executed_flags()\n"));
    struct list_s *list = STM_PSEGMENT->modified_old_objects;
    struct stm_undo_s *undo = (struct stm_undo_s *)list->items;
    struct stm_undo_s *end = (struct stm_undo_s *)(list->items + list->count);

    for (; undo < end; undo++) {
        object_t *obj = undo->object;
        obj->stm_flags |= GCFLAG_WB_EXECUTED;
    }
}




static void _stm_start_transaction(stm_thread_local_t *tl)
{
    assert(!_stm_in_transaction(tl));

    while (!acquire_thread_segment(tl)) {}
    /* GS invalid before this point! */

    assert(STM_PSEGMENT->safe_point == SP_NO_TRANSACTION);
    assert(STM_PSEGMENT->transaction_state == TS_NONE);
    STM_PSEGMENT->transaction_state = TS_REGULAR;
    STM_PSEGMENT->safe_point = SP_RUNNING;
#ifndef NDEBUG
    STM_PSEGMENT->running_pthread = pthread_self();
#endif
    STM_PSEGMENT->shadowstack_at_start_of_transaction = tl->shadowstack;
    STM_PSEGMENT->threadlocal_at_start_of_transaction = tl->thread_local_obj;

    enter_safe_point_if_requested();
    dprintf(("> start_transaction\n"));

    s_mutex_unlock();   // XXX it's probably possible to not acquire this here

    uint8_t old_rv = STM_SEGMENT->transaction_read_version;
    STM_SEGMENT->transaction_read_version = old_rv + 1;
    if (UNLIKELY(old_rv == 0xff)) {
        reset_transaction_read_version();
    }

    assert(list_is_empty(STM_PSEGMENT->modified_old_objects));
    assert(list_is_empty(STM_PSEGMENT->large_overflow_objects));
    assert(list_is_empty(STM_PSEGMENT->objects_pointing_to_nursery));
    assert(list_is_empty(STM_PSEGMENT->young_weakrefs));
    assert(tree_is_cleared(STM_PSEGMENT->young_outside_nursery));
    assert(tree_is_cleared(STM_PSEGMENT->nursery_objects_shadows));
    assert(tree_is_cleared(STM_PSEGMENT->callbacks_on_commit_and_abort[0]));
    assert(tree_is_cleared(STM_PSEGMENT->callbacks_on_commit_and_abort[1]));
    assert(list_is_empty(STM_PSEGMENT->young_objects_with_light_finalizers));
    assert(STM_PSEGMENT->finalizers == NULL);

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
    /* used to be more efficient, starting directly an inevitable transaction,
       but there is no real point any more, I believe */
    rewind_jmp_buf rjbuf;
    stm_rewind_jmp_enterframe(tl, &rjbuf);

    stm_start_transaction(tl);
    stm_become_inevitable(tl, "start_inevitable_transaction");

    stm_rewind_jmp_leaveframe(tl, &rjbuf);
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

    _verify_cards_cleared_in_all_lists(get_priv_segment(STM_SEGMENT->segment_num));
    list_clear(STM_PSEGMENT->objects_pointing_to_nursery);
    list_clear(STM_PSEGMENT->old_objects_with_cards_set);
    list_clear(STM_PSEGMENT->large_overflow_objects);

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
        struct object_s *dst = (struct object_s*)REAL_ADDRESS(segbase, obj);
        assert(dst->stm_flags & GCFLAG_WRITE_BARRIER);
        assert(!(dst->stm_flags & GCFLAG_WB_EXECUTED));
    }
#endif
}

static void push_large_overflow_objects_to_other_segments(void)
{
    if (list_is_empty(STM_PSEGMENT->large_overflow_objects))
        return;

    /* XXX: also pushes small ones right now */
    struct stm_priv_segment_info_s *pseg = get_priv_segment(STM_SEGMENT->segment_num);
    acquire_privatization_lock(STM_SEGMENT->segment_num);
    LIST_FOREACH_R(STM_PSEGMENT->large_overflow_objects, object_t *,
        ({
            assert(!(item->stm_flags & GCFLAG_WB_EXECUTED));            if (obj_should_use_cards(pseg->pub.segment_base, item))
                _reset_object_cards(pseg, item, CARD_CLEAR, false);
            synchronize_object_enqueue(item, true);
        }));
    synchronize_objects_flush();
    release_privatization_lock(STM_SEGMENT->segment_num);

    /* we can as well clear the list here, since the
       objects are only useful if the commit succeeds. And
       we never do a major collection in-between.
       They should also survive any page privatization happening
       before the actual commit, since we always do a pagecopy
       in handle_segfault_in_page() that also copies
       unknown-to-the-segment/uncommitted things.
    */
    list_clear(STM_PSEGMENT->large_overflow_objects);
}


void stm_commit_transaction(void)
{
    exec_local_finalizers();

    assert(!_has_mutex());
    assert(STM_PSEGMENT->safe_point == SP_RUNNING);
    assert(STM_PSEGMENT->running_pthread == pthread_self());

    dprintf(("> stm_commit_transaction()\n"));
    minor_collection(1);

    push_large_overflow_objects_to_other_segments();
    /* push before validate. otherwise they are reachable too early */
    bool was_inev = STM_PSEGMENT->transaction_state == TS_INEVITABLE;
    _validate_and_add_to_commit_log();

    stm_rewind_jmp_forget(STM_SEGMENT->running_thread);

    /* XXX do we still need a s_mutex_lock() section here? */
    s_mutex_lock();
    commit_finalizers();

    /* update 'overflow_number' if needed */
    if (STM_PSEGMENT->overflow_number_has_been_used) {
        highest_overflow_number += GCFLAG_OVERFLOW_NUMBER_bit0;
        assert(highest_overflow_number !=        /* XXX else, overflow! */
               (uint32_t)-GCFLAG_OVERFLOW_NUMBER_bit0);
        STM_PSEGMENT->overflow_number = highest_overflow_number;
        STM_PSEGMENT->overflow_number_has_been_used = false;
    }

    invoke_and_clear_user_callbacks(0);   /* for commit */

    /* >>>>> there may be a FORK() happening in the safepoint below <<<<<*/
    enter_safe_point_if_requested();
    assert(STM_SEGMENT->nursery_end == NURSERY_END);

    /* if a major collection is required, do it here */
    if (is_major_collection_requested()) {
        synchronize_all_threads(STOP_OTHERS_UNTIL_MUTEX_UNLOCK);

        if (is_major_collection_requested()) {   /* if *still* true */
            major_collection_now_at_safe_point();
        }
    }

    _verify_cards_cleared_in_all_lists(get_priv_segment(STM_SEGMENT->segment_num));

    if (globally_unique_transaction && was_inev) {
        committed_globally_unique_transaction();
    }

    /* done */
    stm_thread_local_t *tl = STM_SEGMENT->running_thread;
    _finish_transaction();
    /* cannot access STM_SEGMENT or STM_PSEGMENT from here ! */

    s_mutex_unlock();

    /* between transactions, call finalizers. this will execute
       a transaction itself */
    invoke_general_finalizers(tl);
}

static void reset_modified_from_backup_copies(int segment_num)
{
#pragma push_macro("STM_PSEGMENT")
#pragma push_macro("STM_SEGMENT")
#undef STM_PSEGMENT
#undef STM_SEGMENT
    assert(get_priv_segment(segment_num)->modification_lock);

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

        if (obj_should_use_cards(pseg->pub.segment_base, obj))
            _reset_object_cards(pseg, obj, CARD_CLEAR, false);
        /* XXXXXXXXX: only reset cards of slice!! ^^^^^^^ */

        dprintf(("reset_modified_from_backup_copies(%d): obj=%p off=%lu bk=%p\n",
                 segment_num, obj, SLICE_OFFSET(undo->slice), undo->backup));

        free_bk(undo);
    }

    /* check that all objects have the GCFLAG_WRITE_BARRIER afterwards */
    check_all_write_barrier_flags(pseg->pub.segment_base, list);

    list_clear(list);
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

    abort_finalizers(pseg);

    long bytes_in_nursery = throw_away_nursery(pseg);

    /* some new objects may have cards when aborting, clear them too */
    LIST_FOREACH_R(pseg->new_objects, object_t * /*item*/,
        {
            if (obj_should_use_cards(pseg->pub.segment_base, item))
                _reset_object_cards(pseg, item, CARD_CLEAR, false);
        });

    acquire_modification_lock(segment_num);
    reset_modified_from_backup_copies(segment_num);
    release_modification_lock(segment_num);
    _verify_cards_cleared_in_all_lists(pseg);

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
    tl->thread_local_obj = pseg->threadlocal_at_start_of_transaction;
    tl->last_abort__bytes_in_nursery = bytes_in_nursery;

    list_clear(pseg->objects_pointing_to_nursery);
    list_clear(pseg->old_objects_with_cards_set);
    list_clear(pseg->large_overflow_objects);
    list_clear(pseg->young_weakrefs);
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


void stm_stop_all_other_threads(void)
{
    if (!stm_is_inevitable())         /* may still abort */
        _stm_become_inevitable("stop_all_other_threads");

    s_mutex_lock();
    synchronize_all_threads(STOP_OTHERS_AND_BECOME_GLOBALLY_UNIQUE);
    s_mutex_unlock();
}

void stm_resume_all_other_threads(void)
{
    /* this calls 'committed_globally_unique_transaction()' even though
       we're not committing now.  It's a way to piggyback on the existing
       implementation for stm_become_globally_unique_transaction(). */
    s_mutex_lock();
    committed_globally_unique_transaction();
    s_mutex_unlock();
}



static inline void _synchronize_fragment(stm_char *frag, ssize_t frag_size)
{
    /* double-check that the result fits in one page */
    assert(frag_size > 0);
    assert(frag_size + ((uintptr_t)frag & 4095) <= 4096);

    /* XXX: is it possible to just add to the queue iff the pages
       of the fragment need syncing to other segments? (keep privatization
       lock until the "flush") */

    /* Enqueue this object (or fragemnt of object) */
    if (STM_PSEGMENT->sq_len == SYNC_QUEUE_SIZE)
        synchronize_objects_flush();
    STM_PSEGMENT->sq_fragments[STM_PSEGMENT->sq_len] = frag;
    STM_PSEGMENT->sq_fragsizes[STM_PSEGMENT->sq_len] = frag_size;
    ++STM_PSEGMENT->sq_len;
}



static void synchronize_object_enqueue(object_t *obj)
{
    assert(!_is_young(obj));
    assert(STM_PSEGMENT->privatization_lock);
    assert(obj->stm_flags & GCFLAG_WRITE_BARRIER);
    assert(!(obj->stm_flags & GCFLAG_WB_EXECUTED));

    ssize_t obj_size = stmcb_size_rounded_up(
        (struct object_s *)REAL_ADDRESS(STM_SEGMENT->segment_base, obj));
    OPT_ASSERT(obj_size >= 16);

    if (LIKELY(is_small_uniform(obj))) {
        assert(!(obj->stm_flags & GCFLAG_CARDS_SET));
        OPT_ASSERT(obj_size <= GC_LAST_SMALL_SIZE);
        _synchronize_fragment((stm_char *)obj, obj_size);
        return;
    }

    /* else, a more complicated case for large objects, to copy
       around data only within the needed pages */
    uintptr_t start = (uintptr_t)obj;
    uintptr_t end = start + obj_size;

    do {
        uintptr_t copy_up_to = (start + 4096) & ~4095;   /* end of page */
        if (copy_up_to >= end) {
            copy_up_to = end;        /* this is the last fragment */
        }
        uintptr_t copy_size = copy_up_to - start;

        /* double-check that the result fits in one page */
        assert(copy_size > 0);
        assert(copy_size + (start & 4095) <= 4096);

        _synchronize_fragment((stm_char *)start, copy_size);

        start = copy_up_to;
    } while (start != end);
}

static void synchronize_objects_flush(void)
{
    long j = STM_PSEGMENT->sq_len;
    if (j == 0)
        return;
    STM_PSEGMENT->sq_len = 0;

    dprintf(("synchronize_objects_flush(): %ld fragments\n", j));

    assert(STM_PSEGMENT->privatization_lock);
    DEBUG_EXPECT_SEGFAULT(false);

    long i, myself = STM_SEGMENT->segment_num;
    do {
        --j;
        stm_char *frag = STM_PSEGMENT->sq_fragments[j];
        uintptr_t page = ((uintptr_t)frag) / 4096UL;
        ssize_t frag_size = STM_PSEGMENT->sq_fragsizes[j];

        char *src = REAL_ADDRESS(STM_SEGMENT->segment_base, frag);
        for (i = 0; i < NB_SEGMENTS; i++) {
            if (i == myself)
                continue;

            if (get_page_status_in(i, page) != PAGE_NO_ACCESS) {
                /* shared or private, but never segfault */
                char *dst = REAL_ADDRESS(get_segment_base(i), frag);
                dprintf(("-> flush %p to seg %lu, sz=%lu\n", frag, i, frag_size));
                memcpy(dst, src, frag_size);
            }
        }
    } while (j > 0);

    DEBUG_EXPECT_SEGFAULT(true);
}
