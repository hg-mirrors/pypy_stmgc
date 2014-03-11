import os
import cffi, weakref
from common import parent_dir, source_files

# ----------

ffi = cffi.FFI()
ffi.cdef("""
typedef ... object_t;
typedef ... stm_jmpbuf_t;
#define SIZEOF_MYOBJ ...
#define _STM_FAST_ALLOC ...
#define _STM_GCFLAG_WRITE_BARRIER ...

typedef struct {
    object_t **shadowstack, **shadowstack_base;
    object_t *thread_local_obj;
    char *mem_clear_on_abort;
    size_t mem_bytes_to_clear_on_abort;
    int associated_segment_num;
    ...;
} stm_thread_local_t;

void stm_read(object_t *obj);
/*void stm_write(object_t *obj); use _checked_stm_write() instead */
object_t *stm_allocate(ssize_t size_rounded_up);
object_t *_stm_allocate_old(ssize_t size_rounded_up);

void stm_setup(void);
void stm_teardown(void);
void stm_register_thread_local(stm_thread_local_t *tl);
void stm_unregister_thread_local(stm_thread_local_t *tl);
object_t *stm_setup_prebuilt(object_t *);

bool _checked_stm_write(object_t *obj);
bool _stm_was_read(object_t *obj);
bool _stm_was_written(object_t *obj);
char *_stm_real_address(object_t *obj);
char *_stm_get_segment_base(long index);
bool _stm_in_transaction(stm_thread_local_t *tl);
void _stm_test_switch(stm_thread_local_t *tl);
uint8_t _stm_get_page_flag(uintptr_t index);
int _stm_get_flags(object_t *obj);

void _stm_start_transaction(stm_thread_local_t *tl, stm_jmpbuf_t *jmpbuf);
bool _check_commit_transaction(void);
bool _check_abort_transaction(void);
bool _check_become_inevitable();

void _set_type_id(object_t *obj, uint32_t h);
uint32_t _get_type_id(object_t *obj);
void _set_ptr(object_t *obj, int n, object_t *v);
object_t * _get_ptr(object_t *obj, int n);

void _stm_start_safe_point(void);
bool _check_stop_safe_point(void);

void _stm_set_nursery_free_count(uint64_t free_count);
void _stm_largemalloc_init_arena(char *data_start, size_t data_size);
int _stm_largemalloc_resize_arena(size_t new_size);
char *_stm_largemalloc_data_start(void);
char *_stm_large_malloc(size_t request_size);
void _stm_large_free(char *data);
void _stm_large_dump(void);
void *memset(void *s, int c, size_t n);
bool (*_stm_largemalloc_keep)(char *data);
void _stm_largemalloc_sweep(void);

ssize_t stmcb_size_rounded_up(struct object_s *obj);

long _stm_count_modified_old_objects(void);
long _stm_count_objects_pointing_to_nursery(void);
object_t *_stm_enum_modified_old_objects(long index);
object_t *_stm_enum_objects_pointing_to_nursery(long index);

void stm_collect(long level);
uint64_t _stm_total_allocated(void);

long stm_identityhash(object_t *obj);
long stm_id(object_t *obj);
void stm_set_prebuilt_identityhash(object_t *obj, uint64_t hash);

int stm_can_move(object_t *);
void stm_call_on_abort(stm_thread_local_t *, void *key, void callback(void *));
""")


GC_N_SMALL_REQUESTS = 36      # from gcpage.c
SHARED_PAGE         = 1       # from pages.h
PRIVATE_PAGE        = 3       # from pages.h
LARGE_MALLOC_OVERHEAD = 16    # from largemalloc.h

lib = ffi.verify('''
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

bool _checked_stm_write(object_t *object) {
    stm_jmpbuf_t here;
    stm_segment_info_t *segment = STM_SEGMENT;
    if (__builtin_setjmp(here) == 0) { // returned directly
        assert(segment->jmpbuf_ptr == (stm_jmpbuf_t *)-1);
        segment->jmpbuf_ptr = &here;
        stm_write(object);
        segment->jmpbuf_ptr = (stm_jmpbuf_t *)-1;
        return 0;
    }
    segment->jmpbuf_ptr = (stm_jmpbuf_t *)-1;
    return 1;
}

bool _check_stop_safe_point(void) {
    stm_jmpbuf_t here;
    stm_segment_info_t *segment = STM_SEGMENT;
    if (__builtin_setjmp(here) == 0) { // returned directly
         assert(segment->jmpbuf_ptr == (stm_jmpbuf_t *)-1);
         segment->jmpbuf_ptr = &here;
         _stm_stop_safe_point();
         segment->jmpbuf_ptr = (stm_jmpbuf_t *)-1;
         return 0;
    }
    segment->jmpbuf_ptr = (stm_jmpbuf_t *)-1;
    return 1;
}

bool _check_commit_transaction(void) {
    stm_jmpbuf_t here;
    stm_segment_info_t *segment = STM_SEGMENT;
    if (__builtin_setjmp(here) == 0) { // returned directly
         assert(segment->jmpbuf_ptr == (stm_jmpbuf_t *)-1);
         segment->jmpbuf_ptr = &here;
         stm_commit_transaction();
         segment->jmpbuf_ptr = (stm_jmpbuf_t *)-1;
         return 0;
    }
    segment->jmpbuf_ptr = (stm_jmpbuf_t *)-1;
    return 1;
}

bool _check_abort_transaction(void) {
    stm_jmpbuf_t here;
    stm_segment_info_t *segment = STM_SEGMENT;
    if (__builtin_setjmp(here) == 0) { // returned directly
         assert(segment->jmpbuf_ptr == (stm_jmpbuf_t *)-1);
         segment->jmpbuf_ptr = &here;
         stm_abort_transaction();
         segment->jmpbuf_ptr = (stm_jmpbuf_t *)-1;
         return 0;   // but should be unreachable in this case
    }
    segment->jmpbuf_ptr = (stm_jmpbuf_t *)-1;
    return 1;
}

bool _check_become_inevitable() {
    stm_jmpbuf_t here;
    stm_segment_info_t *segment = STM_SEGMENT;
    if (__builtin_setjmp(here) == 0) { // returned directly
         assert(segment->jmpbuf_ptr == (stm_jmpbuf_t *)-1);
         segment->jmpbuf_ptr = &here;
         stm_become_inevitable("TEST");
         segment->jmpbuf_ptr = (stm_jmpbuf_t *)-1;
         return 0;   // but should be unreachable in this case
    }
    segment->jmpbuf_ptr = (stm_jmpbuf_t *)-1;
    return 1;
}


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
                    ('STM_NO_COND_WAIT', '1'),
                    ('STM_DEBUGPRINT', '1'),
                    ('GC_N_SMALL_REQUESTS', str(GC_N_SMALL_REQUESTS)), #check
                    ],
     undef_macros=['NDEBUG'],
     include_dirs=[parent_dir],
     extra_compile_args=['-g', '-O0', '-Werror', '-ferror-limit=1'],
     force_generic_engine=True)


WORD = 8
HDR = lib.SIZEOF_MYOBJ
assert HDR == 8
GCFLAG_WRITE_BARRIER = lib._STM_GCFLAG_WRITE_BARRIER


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

def stm_allocate(size):
    o = lib.stm_allocate(size)
    tid = 42 + size
    lib._set_type_id(o, tid)
    return o

def stm_allocate_refs(n):
    o = lib.stm_allocate(HDR + n * WORD)
    tid = 421420 + n
    lib._set_type_id(o, tid)
    return o

def stm_set_ref(obj, idx, ref):
    stm_write(obj)
    lib._set_ptr(obj, idx, ref)

def stm_get_ref(obj, idx):
    stm_read(obj)
    return lib._get_ptr(obj, idx)

def stm_set_char(obj, c, offset=HDR):
    stm_write(obj)
    assert HDR <= offset < stm_get_obj_size(obj)
    stm_get_real_address(obj)[offset] = c

def stm_get_char(obj, offset=HDR):
    stm_read(obj)
    assert HDR <= offset < stm_get_obj_size(obj)
    return stm_get_real_address(obj)[offset]

def stm_get_real_address(obj):
    return lib._stm_real_address(ffi.cast('object_t*', obj))

def stm_read(o):
    lib.stm_read(o)

def stm_write(o):
    if lib._checked_stm_write(o):
        raise Conflict()

def stm_was_read(o):
    return lib._stm_was_read(o)

def stm_was_written(o):
    return lib._stm_was_written(o)


def stm_start_safe_point():
    lib._stm_start_safe_point()

def stm_stop_safe_point():
    if lib._check_stop_safe_point():
        raise Conflict()

def stm_become_inevitable():
    if lib._check_become_inevitable():
        raise Conflict()

def stm_minor_collect():
    lib.stm_collect(0)

def stm_major_collect():
    lib.stm_collect(1)

def stm_get_page_flag(pagenum):
    return lib._stm_get_page_flag(pagenum)

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


SHADOWSTACK_LENGTH = 1000
_keepalive = weakref.WeakKeyDictionary()

def _allocate_thread_local():
    tl = ffi.new("stm_thread_local_t *")
    ss = ffi.new("object_t *[]", SHADOWSTACK_LENGTH)
    _keepalive[tl] = ss
    tl.shadowstack = ss
    tl.shadowstack_base = ss
    lib.stm_register_thread_local(tl)
    return tl


class BaseTest(object):

    def setup_method(self, meth):
        lib.stm_setup()
        self.tls = [_allocate_thread_local(), _allocate_thread_local()]
        self.current_thread = 0

    def teardown_method(self, meth):
        for n, tl in enumerate(self.tls):
            if lib._stm_in_transaction(tl):
                if self.current_thread != n:
                    self.switch(n)
                self.abort_transaction()
        for tl in self.tls:
            lib.stm_unregister_thread_local(tl)
        lib.stm_teardown()

    def get_stm_thread_local(self):
        return self.tls[self.current_thread]

    def start_transaction(self):
        tl = self.tls[self.current_thread]
        assert not lib._stm_in_transaction(tl)
        lib._stm_start_transaction(tl, ffi.cast("stm_jmpbuf_t *", -1))
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

    def switch(self, thread_num):
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

    def push_root(self, o):
        assert ffi.typeof(o) == ffi.typeof("object_t *")
        tl = self.tls[self.current_thread]
        curlength = tl.shadowstack - tl.shadowstack_base
        assert 0 <= curlength < SHADOWSTACK_LENGTH
        tl.shadowstack[0] = ffi.cast("object_t *", o)
        tl.shadowstack += 1

    def pop_root(self):
        tl = self.tls[self.current_thread]
        curlength = tl.shadowstack - tl.shadowstack_base
        if curlength == 0:
            raise EmptyStack
        assert 0 < curlength <= SHADOWSTACK_LENGTH
        tl.shadowstack -= 1
        return ffi.cast("object_t *", tl.shadowstack[0])

    def push_root_no_gc(self):
        "Pushes an invalid object, to crash in case the GC is called"
        self.push_root(ffi.cast("object_t *", -1))

    def check_char_everywhere(self, obj, expected_content, offset=HDR):
        for i in range(len(self.tls)):
            addr = lib._stm_get_segment_base(i)
            content = addr[int(ffi.cast("uintptr_t", obj)) + offset]
            assert content == expected_content

    def get_thread_local_obj(self):
        tl = self.tls[self.current_thread]
        return tl.thread_local_obj

    def set_thread_local_obj(self, newobj):
        tl = self.tls[self.current_thread]
        tl.thread_local_obj = newobj
