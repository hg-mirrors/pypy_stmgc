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
void stm_stop_transaction(void);
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

void *memset(void *s, int c, size_t n);
""")

lib = ffi.verify('''
#include <string.h>
#include "core.h"

size_t stm_object_size_rounded_up(object_t * obj) {
    return 16;
}

''', sources=source_files,
     define_macros=[('STM_TESTS', '1')],
     undef_macros=['NDEBUG'],
     include_dirs=[parent_dir],
     extra_compile_args=['-g', '-O0', '-Werror'],
     force_generic_engine=True)


def stm_allocate(size):
    return lib._stm_real_address(lib.stm_allocate(size))

def stm_read(ptr):
    lib.stm_read(lib._stm_tl_address(ptr))

def stm_write(ptr):
    lib.stm_write(lib._stm_tl_address(ptr))

def _stm_was_read(ptr):
    return lib._stm_was_read(lib._stm_tl_address(ptr))

def _stm_was_written(ptr):
    return lib._stm_was_written(lib._stm_tl_address(ptr))

def stm_start_transaction():
    lib.stm_start_transaction()

def stm_stop_transaction(expected_conflict):
    res = lib.stm_stop_transaction()
    if expected_conflict:
        assert res == 0
    else:
        assert res == 1


class BaseTest(object):

    def setup_method(self, meth):
        lib.stm_setup()
        lib.stm_setup_thread()
        self.saved_states = {}
        self.current_proc = "main"

    def teardown_method(self, meth):
        lib._stm_teardown_thread()
        for saved_state in self.saved_states.values():
            lib._stm_restore_local_state(saved_state)
            lib._stm_teardown_thread()
        del self.saved_states
        lib._stm_teardown()

    def switch(self, process_name):
        self.saved_states[self.current_proc] = lib._stm_save_local_state()
        try:
            target_saved_state = self.saved_states.pop(process_name)
        except KeyError:
            lib.stm_setup_thread()
        else:
            lib._stm_restore_local_state(target_saved_state)
        self.current_proc = process_name
