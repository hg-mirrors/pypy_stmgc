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

typedef struct {
...;
} rewind_jmp_thread;

struct stm_shadowentry_s {
    object_t *ss;
};


typedef struct {
    rewind_jmp_thread rjthread;
    struct stm_shadowentry_s *shadowstack, *shadowstack_base;
    char *mem_clear_on_abort;
    size_t mem_bytes_to_clear_on_abort;
    long last_abort__bytes_in_nursery;
    int associated_segment_num;
    struct stm_thread_local_s *prev, *next;
    void *creating_pthread[2];
    ...;
} stm_thread_local_t;

char *stm_object_pages;
char *stm_file_pages;

void stm_read(object_t *obj);
/*void stm_write(object_t *obj); use _checked_stm_write() instead */
object_t *stm_allocate(ssize_t size_rounded_up);

void stm_setup(void);
void stm_teardown(void);
void stm_register_thread_local(stm_thread_local_t *tl);
void stm_unregister_thread_local(stm_thread_local_t *tl);
void stm_validate();
bool _check_stm_validate();

object_t *stm_setup_prebuilt(object_t *);
void _stm_start_safe_point(void);
bool _check_stop_safe_point(void);

ssize_t _checked_stmcb_size_rounded_up(struct object_s *obj);

bool _checked_stm_write(object_t *obj);
bool _stm_was_read(object_t *obj);
bool _stm_was_written(object_t *obj);
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
bool _check_become_globally_unique_transaction(stm_thread_local_t *tl);
int stm_is_inevitable(void);

void _set_type_id(object_t *obj, uint32_t h);
uint32_t _get_type_id(object_t *obj);
void _set_ptr(object_t *obj, int n, object_t *v);
object_t * _get_ptr(object_t *obj, int n);

void stm_collect(long level);
uint64_t _stm_total_allocated(void);

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


long _stm_count_modified_old_objects(void);
long _stm_count_objects_pointing_to_nursery(void);
object_t *_stm_enum_modified_old_objects(long index);
object_t *_stm_enum_objects_pointing_to_nursery(long index);
object_t *_stm_next_last_cl_entry();
void _stm_start_enum_last_cl_entry();

void *memset(void *s, int c, size_t n);

ssize_t stmcb_size_rounded_up(struct object_s *obj);


object_t *_stm_allocate_old_small(ssize_t size_rounded_up);
bool (*_stm_smallmalloc_keep)(char *data);
void _stm_smallmalloc_sweep(void);

""")


GC_N_SMALL_REQUESTS = 36      # from gcpage.c
LARGE_MALLOC_OVERHEAD = 16    # from largemalloc.h

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

bool _check_commit_transaction(void) {
    CHECKED(stm_commit_transaction());
}

long _check_start_transaction(stm_thread_local_t *tl) {
   void **jmpbuf = tl->rjthread.jmpbuf;                         \
    if (__builtin_setjmp(jmpbuf) == 0) { /* returned directly */\
        stm_start_transaction(tl);                              \
        clear_jmpbuf(tl);                                       \
        return 0;                                               \
    }                                                           \
    clear_jmpbuf(tl);                                           \
    return 1;
}

ssize_t _checked_stmcb_size_rounded_up(struct object_s *obj)
{
    stm_thread_local_t *_tl = STM_SEGMENT->running_thread;      \
    void **jmpbuf = _tl->rjthread.jmpbuf;                       \
    if (__builtin_setjmp(jmpbuf) == 0) { /* returned directly */\
        ssize_t res = stmcb_size_rounded_up(obj);
        clear_jmpbuf(_tl);
        return res;
    }
    clear_jmpbuf(_tl);
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

bool _check_become_globally_unique_transaction(stm_thread_local_t *tl) {
    CHECKED(stm_become_globally_unique_transaction(tl, "TESTGUT"));
}

bool _check_stm_validate(void) {
    CHECKED(stm_validate());
}

#undef CHECKED


void _set_type_id(object_t *obj, uint32_t h)
{
    ((myobj_t*)obj)->type_id = h;
}

uint32_t _get_type_id(object_t *obj) {
    return ((myobj_t*)obj)->type_id;
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



ssize_t stmcb_size_rounded_up(struct object_s *obj)
{
    struct myobj_s *myobj = (struct myobj_s*)obj;
    if (myobj->type_id < 421420) {
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
    if (myobj->type_id < 421420) {
        /* basic case: no references */
        return;
    }
    for (i=0; i < myobj->type_id - 421420; i++) {
        object_t **ref = ((object_t **)(myobj + 1)) + i;
        visit(ref);
    }
}


''', sources=source_files,
     define_macros=[('STM_TESTS', '1'),
                    ('STM_NO_AUTOMATIC_SETJMP', '1'),
                    ('STM_LARGEMALLOC_TEST', '1'),
                    ('STM_NO_COND_WAIT', '1'),
                    ('STM_DEBUGPRINT', '1'),
                    ('GC_N_SMALL_REQUESTS', str(GC_N_SMALL_REQUESTS)), #check
                    ],
     undef_macros=['NDEBUG'],
     include_dirs=[parent_dir],
     extra_compile_args=['-g', '-O0', '-Wall', '-Werror', '-ferror-limit=5'],
     extra_link_args=['-g', '-lrt'],
     force_generic_engine=True)


WORD = 8
HDR = lib.SIZEOF_MYOBJ
assert HDR == 8
GCFLAG_WRITE_BARRIER = lib._STM_GCFLAG_WRITE_BARRIER
NB_SEGMENTS = lib.STM_NB_SEGMENTS
FAST_ALLOC = lib._STM_FAST_ALLOC

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

def stm_get_weakref(o):
    return lib._get_weakref(o)

def stm_allocate_refs(n):
    o = lib.stm_allocate(HDR + n * WORD)
    tid = 421420 + n
    lib._set_type_id(o, tid)
    return o

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
    lib.stm_collect(0)

def stm_major_collect():
    lib.stm_collect(1)

def stm_is_accessible_page(pagenum):
    return lib._stm_is_accessible_page(pagenum)

def stm_get_obj_size(o):
    res = lib._checked_stmcb_size_rounded_up(stm_get_real_address(o))
    if res == 1:
        raise Conflict()
    return res

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

def old_objects_with_cards():
    count = lib._stm_count_old_objects_with_cards()
    if count < 0:
        return None
    return map(lib._stm_enum_old_objects_with_cards, range(count))

def last_commit_log_entries():
    lib._stm_start_enum_last_cl_entry()
    res = []
    obj = lib._stm_next_last_cl_entry()
    while obj != ffi.NULL:
        res.append(obj)
        obj = lib._stm_next_last_cl_entry()
    return res



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
        if lib._stm_in_transaction(tl) and lib.stm_is_inevitable():
            self.commit_transaction()      # must succeed!
        #
        for n, tl in enumerate(self.tls):
            if lib._stm_in_transaction(tl):
                if self.current_thread != n:
                    self.switch(n)
                if lib.stm_is_inevitable():
                    self.commit_transaction()   # must succeed!
                else:
                    self.abort_transaction()
        #
        for tl in self.tls:
            lib.stm_unregister_thread_local(tl)
        lib.stm_teardown()

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
                assert tl1.associated_segment_num not in seen
                seen.add(tl1.associated_segment_num)

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
        tl = self.tls[self.current_thread]
        if lib._check_become_globally_unique_transaction(tl):
            raise Conflict()
