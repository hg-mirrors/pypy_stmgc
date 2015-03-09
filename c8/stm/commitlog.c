#ifndef _STM_CORE_H_
# error "must be compiled via stmgc.c"
#endif

uint64_t cle_allocated;

static void setup_commitlog(void)
{
    cle_allocated = 0;
    commit_log_root.next = NULL;
    commit_log_root.segment_num = -1;
    commit_log_root.rev_num = 0;
    commit_log_root.written_count = 0;
}

static void teardown_commitlog(void)
{
    cle_allocated = 0;
    commit_log_root.next = NULL; /* xxx:free them */
    commit_log_root.segment_num = -1;
}

static void add_cle_allocated(ssize_t add_or_remove)
{
    __sync_add_and_fetch(&cle_allocated, add_or_remove);
}

static bool is_cle_collection_requested(void)
{
    return cle_allocated > CLE_COLLECT_BOUND;
}

uint64_t _stm_cle_allocated(void)
{
    return cle_allocated;
}

static char *malloc_bk(size_t bk_size)
{
    add_cle_allocated(bk_size);
    return malloc(bk_size);
}

static void free_bk(struct stm_undo_s *undo)
{
    free(undo->backup);
    assert(undo->backup = (char*)-88);
    add_cle_allocated(-SLICE_SIZE(undo->slice));
}

static struct stm_commit_log_entry_s *malloc_cle(long entries)
{
    size_t byte_len = sizeof(struct stm_commit_log_entry_s) +
        entries * sizeof(struct stm_undo_s);
    struct stm_commit_log_entry_s *result = malloc(byte_len);
    add_cle_allocated(byte_len);
    return result;
}

static void free_cle(struct stm_commit_log_entry_s *e)
{
    dprintf(("free_cle(%p): written=%lu\n", e, e->written_count));
    size_t byte_len = sizeof(struct stm_commit_log_entry_s) +
        e->written_count * sizeof(struct stm_undo_s);
    add_cle_allocated(-byte_len);
    free(e);
}


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



void free_commit_log_entries_up_to(struct stm_commit_log_entry_s *end)
{
    struct stm_commit_log_entry_s *cl, *next;

    cl = &commit_log_root;
    assert(cl->next != NULL && cl->next != INEV_RUNNING);
    assert(end->next != NULL && end->next != INEV_RUNNING);
    assert(cl != end);

    uint64_t rev_num = -1;
    next = cl->next;   /* guaranteed to exist */
    do {
        cl = next;
        rev_num = cl->rev_num;

        /* free bk copies of entries: */
        long count = cl->written_count;
        while (count-->0) {
            free_bk(&cl->written[count]);
        }

        next = cl->next;
        free_cle(cl);

        assert(next != INEV_RUNNING);
        if (cl == end) {
            /* was the last one to free */
            break;
        }
    } while (next != NULL);

    /* set the commit_log_root to the last, common cl entry: */
    commit_log_root.next = next;
    commit_log_root.rev_num = rev_num;
}

void maybe_collect_commit_log()
{
    /* XXX: maybe use other lock, but right now we must make sure
       that we do not run a major GC in parallel, since we do a
       validate in some segments. */
    assert(_has_mutex());

    if (!is_cle_collection_requested())
        return;

    /* do validation in segments with no threads running, as some
       of them may rarely run a thread and thus rarely advance in
       the commit log. */
    int original_num = STM_SEGMENT->segment_num;
    for (long i = 0; i < NB_SEGMENTS; i++) {
        struct stm_priv_segment_info_s *pseg = get_priv_segment(i);

        if (pseg->pub.running_thread == NULL) {
            assert(pseg->transaction_state == TS_NONE);

            set_gs_register(get_segment_base(i));
            bool ok = _stm_validate();
            OPT_ASSERT(IMPLY(STM_PSEGMENT->transaction_state != TS_NONE,
                             ok));
            /* only TS_NONE segments may have stale read-marker data
               that reports a conflict here. but it is fine to ignore it. */
        }
    }
    set_gs_register(get_segment_base(original_num));


    /* look for the last common commit log entry. However,
       don't free the last common one, as it may still be
       used by running threads. */
    struct stm_commit_log_entry_s *cl, *next;

    cl = &commit_log_root;
    next = cl;
    do {
        bool found = false;
        for (int i = 0; i < NB_SEGMENTS; i++) {
            struct stm_commit_log_entry_s *tmp;
            tmp = get_priv_segment(i)->last_commit_log_entry;
            if (next == tmp) {
                found = true;
                break;
            }
        }

        if (found) {
            /* cl is the last one to consider */
            break;
        } else {
            cl = next;
        }
    } while ((next = cl->next) && next != INEV_RUNNING);


    if (cl == &commit_log_root) {
        dprintf(("WARN: triggered CLE collection w/o anything to do\n"));
        return;                 /* none found that is newer */
    }

    /* cl should never be the last (common) one */
    assert(cl->next != NULL && cl->next != INEV_RUNNING);

    free_commit_log_entries_up_to(cl);
}
