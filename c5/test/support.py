import os
import cffi

# ----------

parent_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

header_files = [os.path.join(parent_dir, _n) for _n in
                "core.h pagecopy.h largemalloc.h".split()]
source_files = [os.path.join(parent_dir, _n) for _n in
                "core.c pagecopy.c largemalloc.c".split()]

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
void stm_setup(void);
void stm_setup_process(void);

void stm_start_transaction(void);
_Bool stm_stop_transaction(void);
struct object_s *stm_allocate(size_t size);

void stm_read(struct object_s *object);
void stm_write(struct object_s *object);
_Bool _stm_was_read(struct object_s *object);
_Bool _stm_was_written(struct object_s *object);

struct local_data_s *_stm_save_local_state(void);
void _stm_restore_local_state(struct local_data_s *p);
void _stm_teardown(void);
void _stm_teardown_process(void);

char *stm_large_malloc(size_t request_size);
void stm_large_free(char *data);
void _stm_large_dump(char *data);
void _stm_large_reset(void);

void *memset(void *s, int c, size_t n);
""")

lib = ffi.verify('''
#include <string.h>
#include "core.h"
#include "largemalloc.h"
''', sources=source_files,
     define_macros=[('STM_TESTS', '1')],
     undef_macros=['NDEBUG'],
     include_dirs=[parent_dir],
     extra_compile_args=['-g', '-O0', '-Werror'])

def intptr(p):
    return int(ffi.cast("intptr_t", p))

def stm_allocate(size):
    return ffi.cast("char *", lib.stm_allocate(size))

def stm_read(ptr):
    lib.stm_read(ffi.cast("struct object_s *", ptr))

def stm_write(ptr):
    lib.stm_write(ffi.cast("struct object_s *", ptr))

def _stm_was_read(ptr):
    return lib._stm_was_read(ffi.cast("struct object_s *", ptr))

def _stm_was_written(ptr):
    return lib._stm_was_written(ffi.cast("struct object_s *", ptr))

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
        lib.stm_setup_process()
        self.saved_states = {}
        self.current_proc = "main"

    def teardown_method(self, meth):
        lib._stm_teardown_process()
        for saved_state in self.saved_states.values():
            lib._stm_restore_local_state(saved_state)
            lib._stm_teardown_process()
        del self.saved_states
        lib._stm_teardown()

    def switch(self, process_name):
        self.saved_states[self.current_proc] = lib._stm_save_local_state()
        try:
            target_saved_state = self.saved_states.pop(process_name)
        except KeyError:
            lib.stm_setup_process()
        else:
            lib._stm_restore_local_state(target_saved_state)
        self.current_proc = process_name
