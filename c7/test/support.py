import os
import cffi

# ----------
os.environ['CC'] = 'clang'

parent_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

header_files = [os.path.join(parent_dir, _n) for _n in
                "core.h pagecopy.h list.h".split()]
source_files = [os.path.join(parent_dir, _n) for _n in
                "core.c pagecopy.c list.c".split()]

_pycache_ = os.path.join(parent_dir, 'test', '__pycache__')
if os.path.exists(_pycache_):
    _fs = [_f for _f in os.listdir(_pycache_) if _f.startswith('_cffi_')]
    if _fs:
        _fsmtime = min(os.stat(os.path.join(_pycache_, _f)).st_mtime
                       for _f in _fs)
        if any(os.stat(src).st_mtime >= _fsmtime
               for src in header_files + source_files):
            import shutil
            shutil.rmtree(_pycache_)

# ----------

ffi = cffi.FFI()
ffi.cdef("""
typedef ... object_t;
typedef ... jmpbufptr_t;
#define SIZEOF_MYOBJ ...

void stm_setup(void);
void stm_setup_thread(void);

void stm_start_transaction(jmpbufptr_t *);
bool _stm_stop_transaction(void);
object_t *stm_allocate(size_t size);

void stm_read(object_t *object);
void stm_write(object_t *object);
_Bool _stm_was_read(object_t *object);
_Bool _stm_was_written(object_t *object);

void _stm_restore_local_state(int thread_num);
void _stm_teardown(void);
void _stm_teardown_thread(void);

char *_stm_real_address(object_t *o);
object_t *_stm_tl_address(char *ptr);
bool _stm_is_in_nursery(char *ptr);
object_t *_stm_allocate_old(size_t size);

void _stm_start_safe_point(void);
void _stm_stop_safe_point(void);
bool _stm_check_stop_safe_point(void);

void _set_type_id(object_t *obj, uint32_t h);
uint32_t _get_type_id(object_t *obj);
bool _stm_is_in_transaction(void);

void stm_push_root(object_t *obj);
object_t *stm_pop_root(void);

void _set_ptr(object_t *obj, int n, object_t *v);
object_t * _get_ptr(object_t *obj, int n);

void *memset(void *s, int c, size_t n);
""")

lib = ffi.verify('''
#include <string.h>
#include <assert.h>

#include "core.h"

struct myobj_s {
    struct object_s hdr;
    uint32_t type_id;
};
typedef TLPREFIX struct myobj_s myobj_t;
#define SIZEOF_MYOBJ sizeof(struct myobj_s)

size_t stm_object_size_rounded_up(object_t * obj) {
    return 16;
}

bool _stm_stop_transaction(void) {
    jmpbufptr_t here;
    if (__builtin_setjmp(here) == 0) { // returned directly
         assert(_STM_TL1->jmpbufptr == (jmpbufptr_t*)-1);
         _STM_TL1->jmpbufptr = &here;
         stm_stop_transaction();
         _STM_TL1->jmpbufptr = (jmpbufptr_t*)-1;
         return 0;
    }
    _STM_TL1->jmpbufptr = (jmpbufptr_t*)-1;
    return 1;
}

bool _stm_check_stop_safe_point(void) {
    jmpbufptr_t here;
    if (__builtin_setjmp(here) == 0) { // returned directly
         assert(_STM_TL1->jmpbufptr == (jmpbufptr_t*)-1);
         _STM_TL1->jmpbufptr = &here;
         _stm_stop_safe_point();
         _STM_TL1->jmpbufptr = (jmpbufptr_t*)-1;
         return 0;
    }
    _STM_TL1->jmpbufptr = (jmpbufptr_t*)-1;
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
    localchar_t *field_addr = ((localchar_t*)obj);
    field_addr += SIZEOF_MYOBJ; /* header */
    field_addr += n * sizeof(void*); /* field */
    object_t * TLPREFIX * field = (object_t * TLPREFIX *)field_addr;
    *field = v;
}

object_t * _get_ptr(object_t *obj, int n)
{
    localchar_t *field_addr = ((localchar_t*)obj);
    field_addr += SIZEOF_MYOBJ; /* header */
    field_addr += n * sizeof(void*); /* field */
    object_t * TLPREFIX * field = (object_t * TLPREFIX *)field_addr;
    return *field;
}


size_t stmcb_size(struct object_s *obj)
{
    struct myobj_s *myobj = (struct myobj_s*)obj;
    if (myobj->type_id < 42142) {
        /* basic case: tid equals 42 plus the size of the object */
        assert(myobj->type_id >= 42 + sizeof(struct myobj_s));
        return myobj->type_id - 42;
    }
    else {
        int nrefs = myobj->type_id - 42142;
        assert(nrefs < 100);
        if (nrefs == 0)   /* weakrefs */
            nrefs = 1;
        return sizeof(struct myobj_s) + nrefs * sizeof(void*);
    }
}

void stmcb_trace(struct object_s *obj, void visit(object_t **))
{
    int i;
    struct myobj_s *myobj = (struct myobj_s*)obj;
    if (myobj->type_id < 42142) {
        /* basic case: no references */
        return;
    }
    for (i=0; i < myobj->type_id - 42142; i++) {
        object_t **ref = ((object_t **)(myobj + 1)) + i;
        visit(ref);
    }
}

''', sources=source_files,
     define_macros=[('STM_TESTS', '1')],
     undef_macros=['NDEBUG'],
     include_dirs=[parent_dir],
     extra_compile_args=['-g', '-O0', '-Werror'],
     force_generic_engine=True)


import sys
if sys.maxint > 2**32:
    WORD = 8
else:
    WORD = 4

HDR = lib.SIZEOF_MYOBJ

def is_in_nursery(ptr):
    return lib._stm_is_in_nursery(ptr)

def stm_allocate_old(size):
    o = lib._stm_allocate_old(size)
    tid = 42 + size
    lib._set_type_id(o, tid)
    return o, lib._stm_real_address(o)

def stm_allocate(size):
    o = lib.stm_allocate(size)
    tid = 42 + size
    lib._set_type_id(o, tid)
    return o, lib._stm_real_address(o)

def stm_allocate_refs(n):
    o = lib.stm_allocate(HDR + n * WORD)
    tid = 42142 + n
    lib._set_type_id(o, tid)
    return o, lib._stm_real_address(o)

def stm_set_ref(obj, idx, ref):
    stm_write(obj)
    lib._set_ptr(obj, idx, ref)

def stm_get_ref(obj, idx):
    stm_read(obj)
    return lib._get_ptr(obj, idx)

def stm_get_real_address(obj):
    return lib._stm_real_address(ffi.cast('object_t*', obj))
    
def stm_get_tl_address(ptr):
    return int(ffi.cast('uintptr_t', lib._stm_tl_address(ptr)))

def stm_read(o):
    lib.stm_read(o)

def stm_write(o):
    lib.stm_write(o)

def stm_was_read(o):
    return lib._stm_was_read(o)

def stm_was_written(o):
    return lib._stm_was_written(o)

def stm_push_root(o):
    return lib.stm_push_root(o)

def stm_pop_root():
    return lib.stm_pop_root()

def stm_start_transaction():
    lib.stm_start_transaction(ffi.cast("jmpbufptr_t*", -1))

def stm_stop_transaction(expected_conflict=False):
    res = lib._stm_stop_transaction()
    if expected_conflict:
        assert res == 1
    else:
        assert res == 0

def stm_start_safe_point():
    lib._stm_start_safe_point()

def stm_stop_safe_point(expected_conflict=False):
    res = lib._stm_check_stop_safe_point()
    if expected_conflict:
        assert res == 1
    else:
        assert res == 0


class BaseTest(object):

    def setup_method(self, meth):
        lib.stm_setup()
        lib.stm_setup_thread()
        lib.stm_setup_thread()
        lib._stm_restore_local_state(0)
        self.current_thread = 0

    def teardown_method(self, meth):
        lib._stm_restore_local_state(1)
        if lib._stm_is_in_transaction():
            stm_stop_transaction()
        lib._stm_teardown_thread()
        lib._stm_restore_local_state(0)
        if lib._stm_is_in_transaction():
            stm_stop_transaction()
        lib._stm_teardown_thread()
        lib._stm_teardown()

    def switch(self, thread_num, expect_conflict=False):
        assert thread_num != self.current_thread
        if lib._stm_is_in_transaction():
            stm_start_safe_point()
        lib._stm_restore_local_state(thread_num)
        if lib._stm_is_in_transaction():
            stm_stop_safe_point(expect_conflict)
        elif expect_conflict:
            assert False
        self.current_thread = thread_num
        
