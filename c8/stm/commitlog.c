#ifndef _STM_CORE_H_
# error "must be compiled via stmgc.c"
#endif


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
