#ifndef _STM_CORE_H_
# error "must be compiled via stmgc.c"
# include "core.h"  // silence flymake
#endif



static void setup_signal_handler(void)
{
    struct sigaction act;
    memset(&act, 0, sizeof(act));

	act.sa_sigaction = &_signal_handler;
	/* The SA_SIGINFO flag tells sigaction() to use the sa_sigaction field, not sa_handler. */
	act.sa_flags = SA_SIGINFO | SA_NODEFER;

	if (sigaction(SIGSEGV, &act, NULL) < 0) {
		perror ("sigaction");
		abort();
	}
}


static void copy_bk_objs_in_page_from(int from_segnum, uintptr_t pagenum,
                                      bool only_if_not_modified)
{
    /* looks at all bk copies of objects overlapping page 'pagenum' and
       copies the part in 'pagenum' back to the current segment */
    dprintf(("copy_bk_objs_in_page_from(%d, %ld, %d)\n",
             from_segnum, (long)pagenum, only_if_not_modified));

    assert(modification_lock_check_rdlock(from_segnum));
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
    assert(modification_lock_check_wrlock(STM_SEGMENT->segment_num));
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


static void any_to_accessible(int my_segnum, uintptr_t pagenum)
{
    /* make our page write-ready */
    page_mark_accessible(my_segnum, pagenum);

    /* our READONLY copy *has* to have the current data, no
       copy necessary */
    /* make READONLY pages in other segments NO_ACCESS */
    for (int i = 1; i < NB_SEGMENTS; i++) {
        if (i == my_segnum)
            continue;

        if (get_page_status_in(i, pagenum) == PAGE_READONLY)
            page_mark_inaccessible(i, pagenum);
    }

}

long ro_to_acc = 0;
static void handle_segfault_in_page(uintptr_t pagenum, bool is_write)
{
    /* assumes page 'pagenum' is ACCESS_NONE, privatizes it,
       and validates to newest revision */
    dprintf(("handle_segfault_in_page(%lu), seg %d\n", pagenum, STM_SEGMENT->segment_num));

    /* XXX: bad, but no deadlocks: */
    acquire_all_privatization_locks();

    long i;
    int my_segnum = STM_SEGMENT->segment_num;
    uint8_t page_status = get_page_status_in(my_segnum, pagenum);

    assert(page_status == PAGE_NO_ACCESS
           || page_status == PAGE_READONLY);
    _assert_page_status_invariants(pagenum);

    if (page_status == PAGE_READONLY) {
        /* RO -> ACC */
        assert(is_write);       /* should only fail if linux kernel changed */
        any_to_accessible(my_segnum, pagenum);
        ro_to_acc++;

        /* if was RO, page already has the right contents */
        _assert_page_status_invariants(pagenum);
        release_all_privatization_locks();
        return;

    }

    assert(page_status == PAGE_NO_ACCESS);

    /* if this is just a read-access, try to get a RO view: */
    if (!is_write) {
        bool acc_exists = false;
        for (i = 1; i < NB_SEGMENTS; i++) {
            if (i == my_segnum)
                continue;

            if (get_page_status_in(i, pagenum) == PAGE_ACCESSIBLE) {
                acc_exists = true;
                break;
            }
        }

        if (!acc_exists) {
            /* if there is no ACC version around, it means noone ever had that page
             * ACC since the last major GC -> seg0 has the most current revision and
             * we can get a RO of that. (of course only if this is not a
             * write-access anyway) */

            /* this case could be avoided by making all NO_ACCESS to READONLY
               when resharing pages (XXX: better?).
               We may go from NO_ACCESS->READONLY->ACCESSIBLE */
            dprintf((" > make a previously NO_ACCESS page READONLY\n"));
            page_mark_readonly(my_segnum, pagenum);
            _assert_page_status_invariants(pagenum);
            release_all_privatization_locks();
            return;
        }
    }

    /* find a suitable page to copy from in other segments:
     * suitable means:
     *  - if there is a revision around >= target_rev, get the oldest >= target_rev.
     *  - otherwise find most recent revision
     * Note: simply finding the most recent revision would be a conservative strategy, but
     *       requires going back in time more often (see below)
     */

    /* special case: if there are RO versions around, we want to copy from seg0,
     * since we make RO -> NOACC below before we copy (which wouldn't work). */
    int copy_from_segnum = -1;
    uint64_t copy_from_rev = 0;
    uint64_t target_rev = STM_PSEGMENT->last_commit_log_entry->rev_num;
    for (i = 1; i < NB_SEGMENTS; i++) {
        if (i == my_segnum)
            continue;

        struct stm_commit_log_entry_s *log_entry;
        log_entry = get_priv_segment(i)->last_commit_log_entry;

        /* - if not found anything, initialise copy_from_rev
         * - else if target_rev is higher than everything we found, find newest among them
         * - else: find revision that is as close to target_rev as possible         */
        bool accessible = get_page_status_in(i, pagenum) == PAGE_ACCESSIBLE;
        bool uninit = copy_from_segnum == -1;
        bool find_most_recent = copy_from_rev < target_rev && log_entry->rev_num > copy_from_rev;
        bool find_closest = copy_from_rev >= target_rev && (
            log_entry->rev_num - target_rev < copy_from_rev - target_rev);

        if (accessible && (uninit || find_most_recent || find_closest)) {
            copy_from_segnum = i;
            copy_from_rev = log_entry->rev_num;
            if (copy_from_rev == target_rev)
                break;
        }
    }
    OPT_ASSERT(copy_from_segnum != my_segnum);


    /* make our page write-ready and reconstruct contents */
    any_to_accessible(my_segnum, pagenum);
    _assert_page_status_invariants(pagenum);

    /* account for this page now: XXX */
    /* increment_total_allocated(4096); */

    if (copy_from_segnum == -1) {
        /* this page is only accessible in the sharing segment seg0 so far (new
           allocation). Or it was only in RO pages, which are the same as seg0.
           We can thus simply mark it accessible here w/o undoing any
           modifications or going back in time (seg0 is up-to-date). */
        pagecopy(get_virtual_page(my_segnum, pagenum),
                 get_virtual_page(0, pagenum));
        release_all_privatization_locks();
        return;
    }

    /* before copying anything, acquire modification locks from our and
       the other segment */
    uint64_t to_lock = (1UL << copy_from_segnum);
    acquire_modification_lock_set(to_lock, my_segnum);
    pagecopy(get_virtual_page(my_segnum, pagenum),
             get_virtual_page(copy_from_segnum, pagenum));

    /* if there were modifications in the page, revert them. */
    copy_bk_objs_in_page_from(copy_from_segnum, pagenum, false);

    dprintf(("handle_segfault_in_page: rev %lu to rev %lu\n",
             copy_from_rev, target_rev));
    /* we need to go from 'copy_from_rev' to 'target_rev'.  This
       might need a walk into the past. */
    if (copy_from_rev > target_rev) {
        /* adapt revision of page to our revision:
           if our rev is higher than the page we copy from, everything
           is fine as we never read/modified the page anyway */
        struct stm_commit_log_entry_s *src_version, *target_version;
        src_version = get_priv_segment(copy_from_segnum)->last_commit_log_entry;
        target_version = STM_PSEGMENT->last_commit_log_entry;

        go_to_the_past(pagenum, src_version, target_version);
    }

    release_modification_lock_set(to_lock, my_segnum);
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
        detect_shadowstack_overflow(addr);
        abort();
    }

    int segnum = get_segment_of_linear_address(addr);
    OPT_ASSERT(segnum != 0);
    if (segnum != STM_SEGMENT->segment_num) {
        fprintf(stderr, "Segmentation fault: accessing %p (seg %d) from"
                " seg %d\n", addr, segnum, STM_SEGMENT->segment_num);
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

    // http://stackoverflow.com/questions/17671869/how-to-identify-read-or-write-operations-of-page-fault-when-using-sigaction-hand
    bool is_write = ((ucontext_t*)context)->uc_mcontext.gregs[REG_ERR] & 0x2;

    DEBUG_EXPECT_SEGFAULT(false);
    handle_segfault_in_page(pagenum, is_write);
    DEBUG_EXPECT_SEGFAULT(true);

    errno = saved_errno;
    /* now return and retry */
}
