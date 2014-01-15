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

void *memset(void *s, int c, size_t n);
""")

lib = ffi.verify('''
#include <string.h>
#include <assert.h>

#include "core.h"

size_t stm_object_size_rounded_up(object_t * obj) {
    return 16;
}

bool _stm_stop_transaction(void) {
    jmpbufptr_t here;
    if (__builtin_setjmp(here) == 0) {
         assert(_STM_TL1->jmpbufptr == (jmpbufptr_t*)-1);
         _STM_TL1->jmpbufptr = &here;
         stm_stop_transaction();
         _STM_TL1->jmpbufptr = (jmpbufptr_t*)-1;
         return 0;
    }
    _STM_TL1->jmpbufptr = (jmpbufptr_t*)-1;
    return 1;
}

''', sources=source_files,
     define_macros=[('STM_TESTS', '1')],
     undef_macros=['NDEBUG'],
     include_dirs=[parent_dir],
     extra_compile_args=['-g', '-O0', '-Werror'],
     force_generic_engine=True)


def is_in_nursery(ptr):
    return lib._stm_is_in_nursery(ptr)

def stm_allocate_old(size):
    o = lib._stm_allocate_old(size)
    return o, lib._stm_real_address(o)

def stm_allocate(size):
    o = lib.stm_allocate(size)
    return o, lib._stm_real_address(o)

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

def stm_start_transaction():
    lib.stm_start_transaction(ffi.cast("jmpbufptr_t*", -1))

def stm_stop_transaction(expected_conflict=False):
    res = lib._stm_stop_transaction()
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
        lib._stm_teardown_thread()
        lib._stm_restore_local_state(0)
        lib._stm_teardown_thread()
        lib._stm_teardown()

    def switch(self, thread_num):
        assert thread_num != self.current_thread
        lib._stm_restore_local_state(thread_num)
        self.current_thread = thread_num
        
