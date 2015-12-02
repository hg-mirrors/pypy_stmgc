import cffi, weakref
from common import parent_dir, source_files

# ----------

ffi = cffi.FFI()
ffi.cdef("""
typedef ... object_t;
#define SIZEOF_MYOBJ ...
#define STM_NB_SEGMENTS ...
#define _STM_GCFLAG_WRITE_BARRIER ...
#define _STM_FAST_ALLOC ...
#define _STM_CARD_SIZE ...
#define _STM_CARD_MARKED ...
#define STM_GC_NURSERY ...
#define SIZEOF_HASHTABLE_ENTRY ...

typedef struct {
...;
} rewind_jmp_thread;

struct stm_shadowentry_s {
    object_t *ss;
};


typedef struct {
    rewind_jmp_thread rjthread;
    struct stm_shadowentry_s *shadowstack, *shadowstack_base;
    object_t *thread_local_obj;
    char *mem_clear_on_abort;
    size_t mem_bytes_to_clear_on_abort;
    int last_associated_segment_num;
    struct stm_thread_local_s *prev, *next;
    void *creating_pthread[2];
    ...;
} stm_thread_local_t;

char *stm_object_pages;
char *stm_file_pages;
uintptr_t stm_fill_mark_nursery_bytes;

void stm_read(object_t *obj);
/*void stm_write(object_t *obj); use _checked_stm_write() instead */
object_t *stm_allocate(ssize_t size_rounded_up);
object_t *stm_allocate_weakref(ssize_t size_rounded_up);
object_t *stm_allocate_with_finalizer(ssize_t size_rounded_up);
object_t *stm_allocate_noconflict(ssize_t size_rounded_up);

/*void stm_write_card(); use _checked_stm_write_card() instead */

uint8_t _stm_get_card_value(object_t *obj, long idx);
uint8_t _stm_get_transaction_read_version();

void stm_setup(void);
void stm_teardown(void);
void stm_register_thread_local(stm_thread_local_t *tl);
void stm_unregister_thread_local(stm_thread_local_t *tl);
void stm_validate();
bool _check_stm_validate();

object_t *stm_setup_prebuilt(object_t *);
object_t *stm_setup_prebuilt_weakref(object_t *);

void _stm_start_safe_point(void);
bool _check_stop_safe_point(void);

ssize_t stmcb_size_rounded_up(struct object_s *obj);

bool _checked_stm_write(object_t *obj);
bool _checked_stm_write_card(object_t *obj, uintptr_t index);
bool _stm_was_read(object_t *obj);
bool _stm_was_written(object_t *obj);
bool _stm_was_written_card(object_t *obj);
char *_stm_get_segment_base(long index);
bool _stm_in_transaction(stm_thread_local_t *tl);
int _stm_get_flags(object_t *obj);

object_t *_stm_allocate_old(ssize_t size_rounded_up);
long stm_can_move(object_t *obj);
char *_stm_real_address(object_t *o);
void _stm_test_switch(stm_thread_local_t *tl);
void _stm_test_switch_segment(int segnum);
bool _stm_is_accessible_page(uintptr_t pagenum);

void clear_jmpbuf(stm_thread_local_t *tl);
long _check_start_transaction(stm_thread_local_t *tl);
bool _check_commit_transaction(void);
bool _check_abort_transaction(void);
bool _check_become_inevitable(stm_thread_local_t *tl);
//bool _check_become_globally_unique_transaction(stm_thread_local_t *tl);
bool _check_stop_all_other_threads(void);
void stm_resume_all_other_threads(void);
int stm_is_inevitable(stm_thread_local_t *);
long current_segment_num(void);

void _set_type_id(object_t *obj, uint32_t h);
uint32_t _get_type_id(object_t *obj);
void _set_ptr(object_t *obj, int n, object_t *v);
object_t * _get_ptr(object_t *obj, int n);

void _set_weakref(object_t *obj, object_t *v);
object_t* _get_weakref(object_t *obj);


/* void stm_collect(long level); */
long _check_stm_collect(long level);
uint64_t _stm_total_allocated(void);
uint64_t _stm_cle_allocated(void);

long bytes_before_transaction_break(void);

void _stm_set_nursery_free_count(uint64_t free_count);
void _stm_largemalloc_init_arena(char *data_start, size_t data_size);
int _stm_largemalloc_resize_arena(size_t new_size);
char *_stm_largemalloc_data_start(void);
char *_stm_large_malloc(size_t request_size);
void _stm_large_free(char *data);
void _stm_large_dump(void);
bool (*_stm_largemalloc_keep)(char *data);
void _stm_largemalloc_sweep(void);


long stm_identityhash(object_t *obj);
long stm_id(object_t *obj);
void stm_set_prebuilt_identityhash(object_t *obj, uint64_t hash);


long stm_call_on_abort(stm_thread_local_t *, void *key, void callback(void *));
long stm_call_on_commit(stm_thread_local_t *, void *key, void callback(void *));

/* Profiling events.  In the comments: content of the markers, if any */
enum stm_event_e {
    /* always STM_TRANSACTION_START followed later by one of COMMIT or ABORT */
    STM_TRANSACTION_START,
    STM_TRANSACTION_COMMIT,
    STM_TRANSACTION_ABORT,

    /* inevitable contention: all threads that try to become inevitable
       have a STM_BECOME_INEVITABLE event with a position marker.  Then,
       if it waits it gets a STM_WAIT_OTHER_INEVITABLE.  It is possible
       that a thread gets STM_BECOME_INEVITABLE followed by
       STM_TRANSACTION_ABORT if it fails to become inevitable. */
    STM_BECOME_INEVITABLE,

    /* write-read contention: a "marker" is included in the PYPYSTM file
       saying where the write was done.  Followed by STM_TRANSACTION_ABORT. */
    STM_CONTENTION_WRITE_READ,

    /* always one STM_WAIT_xxx followed later by STM_WAIT_DONE or
       possibly STM_TRANSACTION_ABORT */
    STM_WAIT_FREE_SEGMENT,
    STM_WAIT_SYNCING,
    STM_WAIT_SYNC_PAUSE,
    STM_WAIT_OTHER_INEVITABLE,
    STM_WAIT_DONE,

    /* start and end of GC cycles */
    STM_GC_MINOR_START,
    STM_GC_MINOR_DONE,
    STM_GC_MAJOR_START,
    STM_GC_MAJOR_DONE,
    ...
};

typedef struct {
    uintptr_t odd_number;
    object_t *object;
} stm_loc_marker_t;

typedef void (*stmcb_timing_event_fn)(stm_thread_local_t *tl,
                                      enum stm_event_e event,
                                      stm_loc_marker_t *markers);
stmcb_timing_event_fn stmcb_timing_event;

typedef int (*stm_expand_marker_fn)(char *seg_base, stm_loc_marker_t *marker,
                                    char *output, int output_size);
int stm_set_timing_log(const char *profiling_file_name, int fork_mode,
                       stm_expand_marker_fn expand_marker);

void stm_push_marker(stm_thread_local_t *, uintptr_t, object_t *);
void stm_update_marker_num(stm_thread_local_t *, uintptr_t);
void stm_pop_marker(stm_thread_local_t *);

long _stm_count_modified_old_objects(void);
long _stm_count_objects_pointing_to_nursery(void);
long _stm_count_old_objects_with_cards_set(void);
object_t *_stm_enum_modified_old_objects(long index);
object_t *_stm_enum_objects_pointing_to_nursery(long index);
object_t *_stm_enum_old_objects_with_cards_set(long index);
object_t *_stm_next_last_cl_entry();
void _stm_start_enum_last_cl_entry();
long _stm_count_cl_entries();

void *memset(void *s, int c, size_t n);


object_t *_stm_allocate_old_small(ssize_t size_rounded_up);
bool (*_stm_smallmalloc_keep)(char *data);
void _stm_smallmalloc_sweep_test(void);


void (*stmcb_light_finalizer)(object_t *);
void stm_enable_light_finalizer(object_t *);

void (*stmcb_finalizer)(object_t *);

typedef struct stm_hashtable_s stm_hashtable_t;
typedef ... stm_hashtable_entry_t;
stm_hashtable_t *stm_hashtable_create(void);
void stm_hashtable_free(stm_hashtable_t *);
bool _check_hashtable_read(object_t *, stm_hashtable_t *, uintptr_t key);
object_t *hashtable_read_result;
bool _check_hashtable_write(object_t *, stm_hashtable_t *, uintptr_t key,
                            object_t *nvalue, stm_thread_local_t *tl);
bool _check_hashtable_write_entry(object_t *, stm_hashtable_entry_t *,
                                  object_t *nvalue);
                            
stm_hashtable_entry_t *stm_hashtable_lookup(object_t *hashtableobj,
                                            stm_hashtable_t *hashtable,
                                            uintptr_t index);
long stm_hashtable_length_upper_bound(stm_hashtable_t *);
uint32_t stm_hashtable_entry_userdata;
void stm_hashtable_tracefn(struct object_s *, stm_hashtable_t *,
                           void trace(object_t **));
long _stm_hashtable_list(object_t *o, stm_hashtable_t *h,
                         object_t *entries);
stm_hashtable_entry_t *stm_hashtable_pickitem(object_t *, stm_hashtable_t *);

object_t *_hashtable_iter(stm_hashtable_t *);
stm_hashtable_entry_t *_hashtable_iter_next(object_t *, object_t *);
struct stm_hashtable_table_s *stm_hashtable_iter(stm_hashtable_t *);
stm_hashtable_entry_t **
stm_hashtable_iter_next(object_t *hobj, struct stm_hashtable_table_s *table,
                        stm_hashtable_entry_t **previous);
void stm_hashtable_iter_tracefn(struct stm_hashtable_table_s *table,
                                void trace(object_t **));

void _set_hashtable(object_t *obj, stm_hashtable_t *h);
stm_hashtable_t *_get_hashtable(object_t *obj);
uintptr_t _get_entry_index(stm_hashtable_entry_t *entry);
object_t *_get_entry_object(stm_hashtable_entry_t *entry);
void *_get_hashtable_table(stm_hashtable_t *h);

typedef struct stm_queue_s stm_queue_t;
stm_queue_t *stm_queue_create(void);
void stm_queue_free(stm_queue_t *);
void stm_queue_put(object_t *qobj, stm_queue_t *queue, object_t *newitem);
object_t *stm_queue_get(object_t *qobj, stm_queue_t *queue, double timeout,
                        stm_thread_local_t *tl);
void stm_queue_task_done(stm_queue_t *queue);
int stm_queue_join(object_t *qobj, stm_queue_t *queue, stm_thread_local_t *tl);
void stm_queue_tracefn(stm_queue_t *queue, void trace(object_t **));

void _set_queue(object_t *obj, stm_queue_t *q);
stm_queue_t *_get_queue(object_t *obj);
""")


GC_N_SMALL_REQUESTS = 36      # from smallmalloc.h
GC_LAST_SMALL_SIZE  =   (8 * (GC_N_SMALL_REQUESTS - 1))
LARGE_MALLOC_OVERHEAD = 16    # from largemalloc.h
COMMIT_LOG_ENTRY_OVERHEAD = 0x20 # from core.h
COMMIT_LOG_ENTRY_ENTRY_OVERHEAD = 0x18 # from core.h

lib = ffi.verify(r'''
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "stmgc.h"

struct myobj_s {
    struct object_s hdr;
    uint32_t type_id;
};
typedef TLPREFIX struct myobj_s myobj_t;
#define SIZEOF_MYOBJ sizeof(struct myobj_s)

#define SIZEOF_HASHTABLE_ENTRY sizeof(struct stm_hashtable_entry_s)

int _stm_get_flags(object_t *obj) {
    return obj->stm_flags;
}

void clear_jmpbuf(stm_thread_local_t *tl) {
    memset(&tl->rjthread, 0, sizeof(rewind_jmp_thread));
}

__attribute__((noreturn))
void _test_run_abort(stm_thread_local_t *tl) {
    void **jmpbuf = tl->rjthread.jmpbuf;
    fprintf(stderr, "~~~~~ ABORT ~~~~~\n");
    __builtin_longjmp(jmpbuf, 1);
}

#define CHECKED(CALL)                                           \
    stm_thread_local_t *_tl = STM_SEGMENT->running_thread;      \
    void **jmpbuf = _tl->rjthread.jmpbuf;                       \
    if (__builtin_setjmp(jmpbuf) == 0) { /* returned directly */\
        CALL;                                                   \
        clear_jmpbuf(_tl);                                      \
        return 0;                                               \
    }                                                           \
    clear_jmpbuf(_tl);                                          \
    return 1

bool _checked_stm_write(object_t *object) {
    CHECKED(stm_write(object));
}

bool _checked_stm_write_card(object_t *object, uintptr_t index) {
    CHECKED(stm_write_card(object, index));
}

bool _check_commit_transaction(void) {
    CHECKED(_stm_commit_transaction());
}

bool _check_stm_collect(long level) {
    CHECKED(stm_collect(level));
}

long _check_start_transaction(stm_thread_local_t *tl) {
   void **jmpbuf = tl->rjthread.jmpbuf;                         \
    if (__builtin_setjmp(jmpbuf) == 0) { /* returned directly */\
        stm_enter_transactional_zone(tl);                       \
        clear_jmpbuf(tl);                                       \
        return 0;                                               \
    }                                                           \
    clear_jmpbuf(tl);                                           \
    return 1;
}


bool _check_stop_safe_point(void) {
    CHECKED(_stm_stop_safe_point());
}

bool _check_abort_transaction(void) {
    CHECKED(stm_abort_transaction());
}

bool _check_become_inevitable(stm_thread_local_t *tl) {
    CHECKED(stm_become_inevitable(tl, "TEST"));
}

bool _check_stop_all_other_threads(void) {
    CHECKED(stm_stop_all_other_threads());
}

bool _check_stm_validate(void) {
    CHECKED(stm_validate());
}

object_t *hashtable_read_result;

bool _check_hashtable_read(object_t *hobj, stm_hashtable_t *h, uintptr_t key)
{
    CHECKED(hashtable_read_result = stm_hashtable_read(hobj, h, key));
}

bool _check_hashtable_write(object_t *hobj, stm_hashtable_t *h, uintptr_t key,
                            object_t *nvalue, stm_thread_local_t *tl)
{
    CHECKED(stm_hashtable_write(hobj, h, key, nvalue, tl));
}

bool _check_hashtable_write_entry(object_t *hobj, stm_hashtable_entry_t *entry,
                                  object_t *nvalue)
{
    CHECKED(stm_hashtable_write_entry(hobj, entry, nvalue));
}

#undef CHECKED


void _set_type_id(object_t *obj, uint32_t h)
{
    ((myobj_t*)obj)->type_id = h;
}

uint32_t _get_type_id(object_t *obj) {
    return ((myobj_t*)obj)->type_id;
}

#define WEAKREF_PTR(wr, sz)  ((object_t * TLPREFIX *)(((stm_char *)(wr)) + (sz) - sizeof(void*)))
void _set_weakref(object_t *obj, object_t *v)
{
    char *realobj = _stm_real_address(obj);
    ssize_t size = stmcb_size_rounded_up((struct object_s *)realobj);
    *WEAKREF_PTR(obj, size) = v;
}

object_t * _get_weakref(object_t *obj)
{
    char *realobj = _stm_real_address(obj);
    ssize_t size = stmcb_size_rounded_up((struct object_s *)realobj);
    return *WEAKREF_PTR(obj, size);
}

void _set_hashtable(object_t *obj, stm_hashtable_t *h)
{
    stm_char *field_addr = ((stm_char*)obj);
    field_addr += SIZEOF_MYOBJ; /* header */
    *(stm_hashtable_t *TLPREFIX *)field_addr = h;
}

stm_hashtable_t *_get_hashtable(object_t *obj)
{
    stm_char *field_addr = ((stm_char*)obj);
    field_addr += SIZEOF_MYOBJ; /* header */
    return *(stm_hashtable_t *TLPREFIX *)field_addr;
}

uintptr_t _get_entry_index(stm_hashtable_entry_t *entry)
{
    stm_read((object_t *)entry);
    return entry->index;
}

object_t *_get_entry_object(stm_hashtable_entry_t *entry)
{
    stm_read((object_t *)entry);
    return entry->object;
}


void *_get_hashtable_table(stm_hashtable_t *h) {
    return *((void**)h);
}

long _stm_hashtable_list(object_t *o, stm_hashtable_t *h,
                         object_t *entries)
{
    if (entries != NULL)
        return stm_hashtable_list(o, h,
            (stm_hashtable_entry_t * TLPREFIX*)((stm_char*)entries+SIZEOF_MYOBJ));
    return stm_hashtable_list(o, h, NULL);
}

struct myiter_s {
    struct myobj_s common;
    struct stm_hashtable_table_s *table;
    stm_hashtable_entry_t **previous;
};
typedef TLPREFIX struct myiter_s myiter_t;

object_t *_hashtable_iter(stm_hashtable_t *h)
{
    myiter_t *iter = (myiter_t *)stm_allocate(sizeof(myiter_t));
    _set_type_id(&iter->common.hdr, 421416);
    iter->table = stm_hashtable_iter(h);
    iter->previous = NULL;
    return (object_t *)iter;
}

stm_hashtable_entry_t *_hashtable_iter_next(object_t *hobj, object_t *iterobj)
{
    stm_write(iterobj);
    myiter_t *iter = (myiter_t *)iterobj;
    assert(iter->common.type_id == 421416);
    stm_hashtable_entry_t **pentry;
    pentry = stm_hashtable_iter_next(hobj, iter->table, iter->previous);
    if (pentry == NULL)
        return NULL;
    iter->previous = pentry;
    //fprintf(stderr, "\tcontaining %p: index=%ld, object=%p\n",
    //        *pentry, (*pentry)->index, (*pentry)->object);
    return *pentry;
}


void _set_queue(object_t *obj, stm_queue_t *q)
{
    stm_char *field_addr = ((stm_char*)obj);
    field_addr += SIZEOF_MYOBJ; /* header */
    *(stm_queue_t *TLPREFIX *)field_addr = q;
}

stm_queue_t *_get_queue(object_t *obj)
{
    stm_char *field_addr = ((stm_char*)obj);
    field_addr += SIZEOF_MYOBJ; /* header */
    return *(stm_queue_t *TLPREFIX *)field_addr;
}

void _set_ptr(object_t *obj, int n, object_t *v)
{
    long nrefs = (long)((myobj_t*)obj)->type_id - 421420;
    assert(n < nrefs);

    stm_char *field_addr = ((stm_char*)obj);
    field_addr += SIZEOF_MYOBJ; /* header */
    field_addr += n * sizeof(void*); /* field */
    object_t * TLPREFIX * field = (object_t * TLPREFIX *)field_addr;
    *field = v;
}

object_t * _get_ptr(object_t *obj, int n)
{
    long nrefs = (long)((myobj_t*)obj)->type_id - 421420;
    assert(n < nrefs);

    stm_char *field_addr = ((stm_char*)obj);
    field_addr += SIZEOF_MYOBJ; /* header */
    field_addr += n * sizeof(void*); /* field */
    object_t * TLPREFIX * field = (object_t * TLPREFIX *)field_addr;
    return *field;
}

long bytes_before_transaction_break(void)
{
    return STM_SEGMENT->nursery_mark - STM_SEGMENT->nursery_current;
}


ssize_t stmcb_size_rounded_up(struct object_s *obj)
{
    struct myobj_s *myobj = (struct myobj_s*)obj;
    assert(myobj->type_id != 0);
    if (myobj->type_id < 421420) {
        if (myobj->type_id == 421419) {    /* hashtable */
            return sizeof(struct myobj_s) + 1 * sizeof(void*);
        }
        if (myobj->type_id == 421418) {    /* hashtable entry */
            return sizeof(struct stm_hashtable_entry_s);
        }
        if (myobj->type_id == 421417) {    /* queue */
            return sizeof(struct myobj_s) + 1 * sizeof(void*);
        }
        if (myobj->type_id == 421416) {    /* hashtable iterator */
            return sizeof(struct myobj_s) + 2 * sizeof(void*);
        }
        /* basic case: tid equals 42 plus the size of the object */
        assert(myobj->type_id >= 42 + sizeof(struct myobj_s));
        assert((myobj->type_id - 42) >= 16);
        assert(((myobj->type_id - 42) & 7) == 0);
        return myobj->type_id - 42;
    }
    else {
        uint64_t nrefs = myobj->type_id - 421420;
        assert(nrefs < 10000);     /* artificial limit, to check for garbage */
        if (nrefs == 0)   /* weakrefs */
            nrefs = 1;
        return sizeof(struct myobj_s) + nrefs * sizeof(void*);
    }
}

void stmcb_trace(struct object_s *obj, void visit(object_t **))
{
    int i;
    struct myobj_s *myobj = (struct myobj_s*)obj;
    if (myobj->type_id == 421419) {
        /* hashtable */
        stm_hashtable_t *h = *((stm_hashtable_t **)(myobj + 1));
        stm_hashtable_tracefn(obj, h, visit);
        return;
    }
    if (myobj->type_id == 421418) {
        /* hashtable entry */
        object_t **ref = &((struct stm_hashtable_entry_s *)myobj)->object;
        visit(ref);
    }
    if (myobj->type_id == 421417) {
        /* queue */
        stm_queue_t *q = *((stm_queue_t **)(myobj + 1));
        stm_queue_tracefn(q, visit);
        return;
    }
    if (myobj->type_id == 421416) {
        /* hashtable iterator */
        struct stm_hashtable_table_s *table;
        table = *(struct stm_hashtable_table_s **)(myobj + 1);
        stm_hashtable_iter_tracefn(table, visit);
        return;
    }
    if (myobj->type_id < 421420) {
        /* basic case: no references */
        return;
    }
    for (i=0; i < myobj->type_id - 421420; i++) {
        object_t **ref = ((object_t **)(myobj + 1)) + i;
        visit(ref);
    }
}

long stmcb_obj_supports_cards(struct object_s *obj)
{
    return 1;
}

void stmcb_trace_cards(struct object_s *obj, void visit(object_t **),
                       uintptr_t start, uintptr_t stop)
{
    int i;
    struct myobj_s *myobj = (struct myobj_s*)obj;
    assert(myobj->type_id != 0);
    assert(myobj->type_id != 421419);
    assert(myobj->type_id != 421418);
    assert(myobj->type_id != 421417);
    assert(myobj->type_id != 421416);
    if (myobj->type_id < 421420) {
        /* basic case: no references */
        return;
    }

    for (i=start; (i < myobj->type_id - 421420) && (i < stop); i++) {
        object_t **ref = ((object_t **)(myobj + 1)) + i;
        visit(ref);
    }
}

void stmcb_get_card_base_itemsize(struct object_s *obj,
                                  uintptr_t offset_itemsize[2])
{
    struct myobj_s *myobj = (struct myobj_s*)obj;
    assert(myobj->type_id != 0);
    assert(myobj->type_id != 421419);
    assert(myobj->type_id != 421418);
    assert(myobj->type_id != 421417);
    assert(myobj->type_id != 421416);
    if (myobj->type_id < 421420) {
        offset_itemsize[0] = SIZEOF_MYOBJ;
        offset_itemsize[1] = 1;
    }
    else {
        offset_itemsize[0] = sizeof(struct myobj_s);
        offset_itemsize[1] = sizeof(object_t *);
    }
}

void stm_push_marker(stm_thread_local_t *tl, uintptr_t onum, object_t *ob)
{
    STM_PUSH_MARKER(*tl, onum, ob);
}

void stm_update_marker_num(stm_thread_local_t *tl, uintptr_t onum)
{
    STM_UPDATE_MARKER_NUM(*tl, onum);
}

void stm_pop_marker(stm_thread_local_t *tl)
{
    STM_POP_MARKER(*tl);
}

long current_segment_num(void)
{
    return STM_SEGMENT->segment_num;
}
''', sources=source_files,
     define_macros=[('STM_TESTS', '1'),
                    ('STM_NO_AUTOMATIC_SETJMP', '1'),
                    ('STM_LARGEMALLOC_TEST', '1'),
                    ('STM_NO_COND_WAIT', '1'),
                    ('STM_DEBUGPRINT', '1'),
                    ('_STM_NURSERY_ZEROED', '1'),
                    ('STM_GC_NURSERY', '128'), # KB
                    ('GC_N_SMALL_REQUESTS', str(GC_N_SMALL_REQUESTS)), #check
                    ],
     undef_macros=['NDEBUG'],
     include_dirs=[parent_dir],
                 extra_compile_args=['-g', '-O0', '-Werror', '-Wall'], #, '-ferror-limit=5'],
     extra_link_args=['-g', '-lrt'],
     force_generic_engine=True)


WORD = 8
HDR = lib.SIZEOF_MYOBJ
assert HDR == 8
GCFLAG_WRITE_BARRIER = lib._STM_GCFLAG_WRITE_BARRIER
NB_SEGMENTS = lib.STM_NB_SEGMENTS
FAST_ALLOC = lib._STM_FAST_ALLOC
CARD_SIZE = lib._STM_CARD_SIZE # 16b at least
CARD_CLEAR = 0
CARD_MARKED = lib._STM_CARD_MARKED
CARD_MARKED_OLD = lib._stm_get_transaction_read_version
lib.stm_hashtable_entry_userdata = 421418
NURSERY_SIZE = lib.STM_GC_NURSERY * 1024 # bytes
SIZEOF_HASHTABLE_ENTRY = lib.SIZEOF_HASHTABLE_ENTRY

class Conflict(Exception):
    pass

class EmptyStack(Exception):
    pass

def is_in_nursery(o):
    return lib.stm_can_move(o)

def stm_allocate_old(size):
    o = lib._stm_allocate_old(size)
    tid = 42 + size
    lib._set_type_id(o, tid)
    return o

def stm_allocate_old_refs(n):
    o = lib._stm_allocate_old(HDR + n * WORD)
    tid = 421420 + n
    lib._set_type_id(o, tid)
    return o

def stm_allocate_old_small(size):
    o = lib._stm_allocate_old_small(size)
    tid = 42 + size
    lib._set_type_id(o, tid)
    return o

def stm_allocate(size):
    o = lib.stm_allocate(size)
    tid = 42 + size
    lib._set_type_id(o, tid)
    return o

def stm_allocate_weakref(point_to_obj, size=None):
    assert HDR+WORD == 16
    o = lib.stm_allocate_weakref(HDR + WORD)

    tid = 421420
    lib._set_type_id(o, tid)
    lib._set_weakref(o, point_to_obj)
    return o

def stm_allocate_refs(n):
    o = lib.stm_allocate(HDR + n * WORD)
    tid = 421420 + n
    lib._set_type_id(o, tid)
    return o

def stm_allocate_noconflict(size):
    o = lib.stm_allocate_noconflict(size)
    tid = 42 + size
    lib._set_type_id(o, tid)
    return o

def stm_allocate_noconflict_refs(n):
    o = lib.stm_allocate_noconflict(HDR + n * WORD)
    tid = 421420 + n
    lib._set_type_id(o, tid)
    return o

def stm_allocate_with_finalizer(size):
    o = lib.stm_allocate_with_finalizer(size)
    tid = 42 + size
    lib._set_type_id(o, tid)
    return o

def stm_allocate_with_finalizer_refs(n):
    o = lib.stm_allocate_with_finalizer(HDR + n * WORD)
    tid = 421420 + n
    lib._set_type_id(o, tid)
    return o

SIZEOF_HASHTABLE_OBJ = 16 + lib.SIZEOF_MYOBJ
def stm_allocate_hashtable():
    o = lib.stm_allocate(16)
    assert is_in_nursery(o)
    tid = 421419
    lib._set_type_id(o, tid)
    h = lib.stm_hashtable_create()
    lib._set_hashtable(o, h)
    return o

def hashtable_iter(hobj):
    return lib._hashtable_iter(get_hashtable(hobj))

def hashtable_iter_next(hobj, o):
    return lib._hashtable_iter_next(hobj, o)

def hashtable_lookup(hto, ht, idx):
    return ffi.cast("object_t*",
                    lib.stm_hashtable_lookup(hto, ht, idx))

def get_hashtable(o):
    assert lib._get_type_id(o) == 421419
    h = lib._get_hashtable(o)
    assert h
    return h

def stm_allocate_queue():
    o = lib.stm_allocate(16)
    tid = 421417
    lib._set_type_id(o, tid)
    q = lib.stm_queue_create()
    lib._set_queue(o, q)
    return o

def get_queue(o):
    assert lib._get_type_id(o) == 421417
    q = lib._get_queue(o)
    assert q
    return q

def stm_get_weakref(o):
    return lib._get_weakref(o)

def stm_set_ref(obj, idx, ref, use_cards=False):
    if use_cards:
        stm_write_card(obj, idx)
    else:
        stm_write(obj)
    lib._set_ptr(obj, idx, ref)

def stm_get_ref(obj, idx):
    stm_read(obj)
    return lib._get_ptr(obj, idx)

def stm_set_char(obj, c, offset=HDR, use_cards=False):
    assert HDR <= offset < stm_get_obj_size(obj)
    if use_cards:
        stm_write_card(obj, offset - HDR)
    else:
        stm_write(obj)
    stm_get_real_address(obj)[offset] = c

def stm_get_char(obj, offset=HDR):
    assert HDR <= offset < stm_get_obj_size(obj)
    stm_read(obj)
    return stm_get_real_address(obj)[offset]

def stm_get_real_address(obj):
    return lib._stm_real_address(ffi.cast('object_t*', obj))

def stm_read(o):
    lib.stm_read(o)


def stm_write(o):
    if lib._checked_stm_write(o):
        raise Conflict()

def stm_write_card(o, index):
    if lib._checked_stm_write_card(o, index):
        raise Conflict()

def stm_was_read(o):
    return lib._stm_was_read(o)

def stm_was_written(o):
    return lib._stm_was_written(o)

def stm_was_written_card(o):
    return lib._stm_was_written_card(o)

def stm_validate():
    if lib._check_stm_validate():
        raise Conflict()

def stm_start_safe_point():
    lib._stm_start_safe_point()

def stm_stop_safe_point():
    if lib._check_stop_safe_point():
        raise Conflict()

def stm_minor_collect():
    assert not lib._check_stm_collect(0) # no conflict

def stm_major_collect():
    res = lib._check_stm_collect(1)
    if res == 1:
        raise Conflict()
    return res

def stm_is_accessible_page(pagenum):
    return lib._stm_is_accessible_page(pagenum)

def stm_get_obj_size(o):
    return lib.stmcb_size_rounded_up(stm_get_real_address(o))

def stm_get_obj_pages(o):
    start = int(ffi.cast('uintptr_t', o))
    startp = start // 4096
    return range(startp, startp + stm_get_obj_size(o) // 4096 + 1)

def stm_get_flags(o):
    return lib._stm_get_flags(o)

def modified_old_objects():
    count = lib._stm_count_modified_old_objects()
    if count < 0:
        return None
    return map(lib._stm_enum_modified_old_objects, range(count))

def objects_pointing_to_nursery():
    count = lib._stm_count_objects_pointing_to_nursery()
    if count < 0:
        return None
    return map(lib._stm_enum_objects_pointing_to_nursery, range(count))

def old_objects_with_cards_set():
    count = lib._stm_count_old_objects_with_cards_set()
    if count < 0:
        return None
    return map(lib._stm_enum_old_objects_with_cards_set, range(count))

def last_commit_log_entry_objs():
    lib._stm_start_enum_last_cl_entry()
    res = []
    obj = lib._stm_next_last_cl_entry()
    while obj != ffi.NULL:
        res.append(obj)
        obj = lib._stm_next_last_cl_entry()
    return res

def count_commit_log_entries():
    return lib._stm_count_cl_entries()



SHADOWSTACK_LENGTH = 1000
_keepalive = weakref.WeakKeyDictionary()

def _allocate_thread_local():
    tl = ffi.new("stm_thread_local_t *")
    ss = ffi.new("struct stm_shadowentry_s[]", SHADOWSTACK_LENGTH)
    _keepalive[tl] = ss
    tl.shadowstack = ss
    tl.shadowstack_base = ss
    lib.stm_register_thread_local(tl)
    return tl


class BaseTest(object):
    NB_THREADS = 4

    def setup_method(self, meth):
        lib.stm_setup()
        self.tls = [_allocate_thread_local() for i in range(self.NB_THREADS)]
        self.current_thread = 0
        # force-switch back to segment 0 so that when we do something
        # outside of transactions before the test, it happens in sharing seg0
        lib._stm_test_switch_segment(-1)

    def teardown_method(self, meth):
        lib.stmcb_expand_marker = ffi.NULL
        lib.stmcb_debug_print = ffi.NULL
        tl = self.tls[self.current_thread]
        if lib._stm_in_transaction(tl) and self.is_inevitable():
            self.commit_transaction()      # must succeed!
        #
        for n, tl in enumerate(self.tls):
            if lib._stm_in_transaction(tl):
                if self.current_thread != n:
                    self.switch(n)
                if self.is_inevitable():
                    self.commit_transaction()   # must succeed!
                else:
                    self.abort_transaction()
        #
        for tl in self.tls:
            lib.stm_unregister_thread_local(tl)
        lib.stm_teardown()

    def is_inevitable(self):
        tl = self.tls[self.current_thread]
        assert lib._stm_in_transaction(tl)
        return lib.stm_is_inevitable(tl)

    def get_stm_thread_local(self):
        return self.tls[self.current_thread]

    def start_transaction(self):
        tl = self.tls[self.current_thread]
        assert not lib._stm_in_transaction(tl)
        if lib._check_start_transaction(tl):
            raise Conflict()
        lib.clear_jmpbuf(tl)
        assert lib._stm_in_transaction(tl)
        #
        seen = set()
        for tl1 in self.tls:
            if lib._stm_in_transaction(tl1):
                assert tl1.last_associated_segment_num not in seen
                seen.add(tl1.last_associated_segment_num)

    def commit_transaction(self):
        tl = self.tls[self.current_thread]
        assert lib._stm_in_transaction(tl)
        res = lib._check_commit_transaction()
        assert not lib._stm_in_transaction(tl)
        if res:
            raise Conflict

    def abort_transaction(self):
        tl = self.tls[self.current_thread]
        assert lib._stm_in_transaction(tl)
        res = lib._check_abort_transaction()
        assert res   # abort_transaction() didn't abort!
        assert not lib._stm_in_transaction(tl)

    def switch(self, thread_num, validate=True):
        assert thread_num != self.current_thread
        tl = self.tls[self.current_thread]
        if lib._stm_in_transaction(tl):
            stm_start_safe_point()
        #
        self.current_thread = thread_num
        tl2 = self.tls[thread_num]
        #
        if lib._stm_in_transaction(tl2):
            lib._stm_test_switch(tl2)
            stm_stop_safe_point() # can raise Conflict
            if validate:
                stm_validate() # can raise Conflict

    def switch_to_segment(self, seg_num):
        lib._stm_test_switch_segment(seg_num)

    def push_roots(self, os):
        for o in os:
            self.push_root(o)
        self._last_push_all = os

    def pop_roots(self):
        os = self._last_push_all
        self._last_push_all = None
        return list(reversed([self.pop_root() for _ in os]))


    def push_root(self, o):
        assert ffi.typeof(o) == ffi.typeof("object_t *")
        tl = self.tls[self.current_thread]
        curlength = tl.shadowstack - tl.shadowstack_base
        assert 0 <= curlength < SHADOWSTACK_LENGTH
        tl.shadowstack[0].ss = ffi.cast("object_t *", o)
        tl.shadowstack += 1

    def pop_root(self):
        tl = self.tls[self.current_thread]
        curlength = tl.shadowstack - tl.shadowstack_base
        assert curlength >= 1
        if curlength == 1:
            raise EmptyStack
        assert 0 < curlength <= SHADOWSTACK_LENGTH
        tl.shadowstack -= 1
        return ffi.cast("object_t *", tl.shadowstack[0].ss)

    def push_root_no_gc(self):
        "Pushes an invalid object, to crash in case the GC is called"
        self.push_root(ffi.cast("object_t *", 8))

    def check_char_everywhere(self, obj, expected_content, offset=HDR):
        for i in range(self.NB_THREADS):
            if self.current_thread != i:
                self.switch(i)
            tl = self.tls[i]
            if not lib._stm_in_transaction(tl):
                self.start_transaction()

            # check:
            addr = lib._stm_get_segment_base(i)
            content = addr[int(ffi.cast("uintptr_t", obj)) + offset]
            assert content == expected_content

            self.abort_transaction()

    def get_thread_local_obj(self):
        tl = self.tls[self.current_thread]
        return tl.thread_local_obj

    def set_thread_local_obj(self, newobj):
        tl = self.tls[self.current_thread]
        tl.thread_local_obj = newobj

    def become_inevitable(self):
        tl = self.tls[self.current_thread]
        if lib._check_become_inevitable(tl):
            raise Conflict()

    def become_globally_unique_transaction(self):
        import py; py.test.skip("this function was removed")
        tl = self.tls[self.current_thread]
        if lib._check_become_globally_unique_transaction(tl):
            raise Conflict()

    def stop_all_other_threads(self):
        if lib._check_stop_all_other_threads():
            raise Conflict()

    def resume_all_other_threads(self):
        if lib.stm_resume_all_other_threads():
            raise Conflict()
