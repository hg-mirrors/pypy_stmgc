
static void _timing_record_write(void);
static void timing_write_read_contention(object_t *obj);


#define timing_event(tl, event)                                         \
    (stmcb_timing_event != NULL ? stmcb_timing_event(tl, event, NULL) : (void)0)

#define timing_record_write()                                           \
    (stmcb_timing_event != NULL ? _timing_record_write() : (void)0)
