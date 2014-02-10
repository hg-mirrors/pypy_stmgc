import os
import cffi
import sys
assert sys.maxint == 9223372036854775807, "requires a 64-bit environment"

# ----------
os.environ['CC'] = 'clang'

parent_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

source_files = [os.path.join(parent_dir, "stmgc.c")]
all_files = [os.path.join(parent_dir, "stmgc.h"),
             os.path.join(parent_dir, "stmgc.c")] + [
    os.path.join(parent_dir, 'stm', _n)
        for _n in os.listdir(os.path.join(parent_dir, 'stm'))
            if _n.endswith('.h') or _n.endswith('.c')]

_pycache_ = os.path.join(parent_dir, 'test', '__pycache__')
if os.path.exists(_pycache_):
    _fs = [_f for _f in os.listdir(_pycache_) if _f.startswith('_cffi_')]
    if _fs:
        _fsmtime = min(os.stat(os.path.join(_pycache_, _f)).st_mtime
                       for _f in _fs)
        if any(os.stat(src).st_mtime >= _fsmtime for src in all_files):
            import shutil
            shutil.rmtree(_pycache_)

# ----------

ffi = cffi.FFI()
ffi.cdef("""
typedef ... object_t;
typedef ... stm_jmpbuf_t;
#define SIZEOF_MYOBJ ...

typedef struct {
    object_t **shadowstack, **shadowstack_base;
    ...;
} stm_thread_local_t;

void stm_read(object_t *obj);
/*void stm_write(object_t *obj); use _checked_stm_write() instead */
object_t *stm_allocate(ssize_t size_rounded_up);
object_t *stm_allocate_prebuilt(ssize_t size_rounded_up);

void stm_setup(void);
void stm_teardown(void);

bool _checked_stm_write(object_t *obj);
bool _stm_was_read(object_t *obj);
bool _stm_was_written(object_t *obj);

void stm_register_thread_local(stm_thread_local_t *tl);
void stm_unregister_thread_local(stm_thread_local_t *tl);

void stm_start_transaction(stm_thread_local_t *tl, stm_jmpbuf_t *jmpbuf);
""")


TEMPORARILY_DISABLED = """
void stm_start_inevitable_transaction(stm_thread_local_t *tl);
void stm_commit_transaction(void);
void stm_abort_transaction(void);
void stm_become_inevitable(char* msg);
bool _stm_in_nursery(object_t *obj);
char *_stm_real_address(object_t *obj);
object_t *_stm_segment_address(char *ptr);

void _stm_start_safe_point(uint8_t);
void _stm_stop_safe_point(uint8_t);
bool _stm_check_stop_safe_point(void);

void _set_type_id(object_t *obj, uint32_t h);
uint32_t _get_type_id(object_t *obj);
bool _stm_is_in_transaction(void);

void stm_push_root(object_t *obj);
object_t *stm_pop_root(void);

void _set_ptr(object_t *obj, int n, object_t *v);
object_t * _get_ptr(object_t *obj, int n);

void _stm_minor_collect();

bool _stm_check_abort_transaction(void);

void *memset(void *s, int c, size_t n);
extern size_t stmcb_size(struct object_s *);
extern void stmcb_trace(struct object_s *, void (object_t **));

uint8_t _stm_get_flags(object_t *obj);
uint8_t stm_get_page_flag(int pagenum);
enum {
    SHARED_PAGE=0,
    REMAPPING_PAGE,
    PRIVATE_PAGE,
};  /* flag_page_private */

enum {
    GCFLAG_WRITE_BARRIER = 1,
    GCFLAG_NOT_COMMITTED = 2,
    GCFLAG_MOVED = 4,
};

enum {
    LOCK_COLLECT = 1,
    LOCK_EXCLUSIVE = 2,
    THREAD_YIELD = 4,
};


void stm_largemalloc_init(char *data_start, size_t data_size);
int stm_largemalloc_resize_arena(size_t new_size);

object_t *stm_large_malloc(size_t request_size);
void stm_large_free(object_t *data);

void _stm_large_dump(void);
char *_stm_largemalloc_data_start(void);

void _stm_move_object(object_t* obj, char *src, char *dst);
size_t _stm_data_size(struct object_s *data);
void _stm_chunk_pages(struct object_s *data, uintptr_t *start, uintptr_t *num);

void stm_become_inevitable(char* msg);
void stm_start_inevitable_transaction();
bool _checked_stm_become_inevitable();
"""


lib = ffi.verify('''
#include <string.h>
#include <assert.h>

#include "stmgc.h"

struct myobj_s {
    struct object_s hdr;
    uint32_t type_id;
};
typedef TLPREFIX struct myobj_s myobj_t;
#define SIZEOF_MYOBJ sizeof(struct myobj_s)


uint8_t _stm_get_flags(object_t *obj) {
    return obj->stm_flags;
}

#if 0
bool _checked_stm_become_inevitable() {
    jmpbufptr_t here;
    int tn = _STM_TL->thread_num;
    if (__builtin_setjmp(here) == 0) { // returned directly
         assert(_STM_TL->jmpbufptr == (jmpbufptr_t*)-1);
         _STM_TL->jmpbufptr = &here;
         stm_become_inevitable("TEST");
         _STM_TL->jmpbufptr = (jmpbufptr_t*)-1;
         return 0;
    }
    _stm_dbg_get_tl(tn)->jmpbufptr = (jmpbufptr_t*)-1;
    return 1;
}
#endif

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

#if 0
bool _stm_stop_transaction(void) {
    jmpbufptr_t here;
    int tn = _STM_TL->thread_num;
    if (__builtin_setjmp(here) == 0) { // returned directly
         assert(_STM_TL->jmpbufptr == (jmpbufptr_t*)-1);
         _STM_TL->jmpbufptr = &here;
         stm_stop_transaction();
         _stm_dbg_get_tl(tn)->jmpbufptr = (jmpbufptr_t*)-1;
         return 0;
    }
    _stm_dbg_get_tl(tn)->jmpbufptr = (jmpbufptr_t*)-1;
    return 1;
}

bool _stm_check_stop_safe_point(void) {
    jmpbufptr_t here;
    int tn = _STM_TL->thread_num;
    if (__builtin_setjmp(here) == 0) { // returned directly
         assert(_STM_TL->jmpbufptr == (jmpbufptr_t*)-1);
         _STM_TL->jmpbufptr = &here;
         _stm_stop_safe_point(LOCK_COLLECT);
         _STM_TL->jmpbufptr = (jmpbufptr_t*)-1;
         return 0;
    }
    _stm_dbg_get_tl(tn)->jmpbufptr = (jmpbufptr_t*)-1;
    return 1;
}

bool _stm_check_abort_transaction(void) {
    jmpbufptr_t here;
    int tn = _STM_TL->thread_num;
    if (__builtin_setjmp(here) == 0) { // returned directly
         assert(_STM_TL->jmpbufptr == (jmpbufptr_t*)-1);
         _STM_TL->jmpbufptr = &here;
         stm_abort_transaction();
         _stm_dbg_get_tl(tn)->jmpbufptr = (jmpbufptr_t*)-1;
         return 0;
    }
    _stm_dbg_get_tl(tn)->jmpbufptr = (jmpbufptr_t*)-1;
    return 1;
}
#endif


void _set_type_id(object_t *obj, uint32_t h)
{
    ((myobj_t*)obj)->type_id = h;
}

uint32_t _get_type_id(object_t *obj) {
    return ((myobj_t*)obj)->type_id;
}


void _set_ptr(object_t *obj, int n, object_t *v)
{
    stm_char *field_addr = ((stm_char*)obj);
    field_addr += SIZEOF_MYOBJ; /* header */
    field_addr += n * sizeof(void*); /* field */
    object_t * TLPREFIX * field = (object_t * TLPREFIX *)field_addr;
    *field = v;
}

object_t * _get_ptr(object_t *obj, int n)
{
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
        int nrefs = myobj->type_id - 421420;
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
     define_macros=[('STM_TESTS', '1')],
     undef_macros=['NDEBUG'],
     include_dirs=[parent_dir],
     extra_compile_args=['-g', '-O0', '-Werror'],
     force_generic_engine=True)


WORD = 8
HDR = lib.SIZEOF_MYOBJ
assert HDR == 8

class Conflict(Exception):
    pass

def is_in_nursery(o):
    return lib._stm_in_nursery(o)

def stm_allocate_old(size):
    o = lib._stm_allocate_old(size)
    tid = 42 + size
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

def stm_set_char(obj, c):
    stm_write(obj)
    stm_get_real_address(obj)[HDR] = c

def stm_get_char(obj):
    stm_read(obj)
    return stm_get_real_address(obj)[HDR]

def stm_get_real_address(obj):
    return lib._stm_real_address(ffi.cast('object_t*', obj))
    
def stm_get_segment_address(ptr):
    return int(ffi.cast('uintptr_t', lib._stm_segment_address(ptr)))

def stm_read(o):
    lib.stm_read(o)

def stm_write(o):
    if lib._checked_stm_write(o):
        raise Conflict()

def stm_was_read(o):
    return lib._stm_was_read(o)

def stm_was_written(o):
    return lib._stm_was_written(o)

def stm_push_root(o):
    return lib.stm_push_root(o)

def stm_pop_root():
    return lib.stm_pop_root()

def stm_stop_transaction():
    if lib._stm_stop_transaction():
        raise Conflict()

def stm_abort_transaction():
    return lib._stm_check_abort_transaction()


def stm_start_safe_point():
    lib._stm_start_safe_point(lib.LOCK_COLLECT)

def stm_stop_safe_point():
    if lib._stm_check_stop_safe_point():
        raise Conflict()

def stm_become_inevitable():
    if lib._checked_stm_become_inevitable():
        raise Conflict()

def stm_minor_collect():
    lib._stm_minor_collect()

def stm_get_page_flag(pagenum):
    return lib.stm_get_page_flag(pagenum)

def stm_get_obj_size(o):
    return lib.stmcb_size(stm_get_real_address(o))

def stm_get_obj_pages(o):
    start = int(ffi.cast('uintptr_t', o))
    startp = start // 4096
    return range(startp, startp + stm_get_obj_size(o) // 4096 + 1)

def stm_get_flags(o):
    return lib._stm_get_flags(o)

def _allocate_thread_local():
    tl = ffi.new("stm_thread_local_t *")
    lib.stm_register_thread_local(tl)
    return tl


class BaseTest(object):

    def setup_method(self, meth):
        lib.stm_setup()
        self.tls = [_allocate_thread_local(), _allocate_thread_local()]
        self.current_thread = 0
        self.running_transaction = set()

    def teardown_method(self, meth):
        for n in sorted(self.running_transaction):
            self.switch(n)
            self.abort_transaction()
        for tl in self.tls:
            lib.stm_unregister_thread_local(tl)
        lib.stm_teardown()

    def start_transaction(self):
        n = self.current_thread
        assert n not in self.running_transaction
        tl = self.tls[n]
        lib.stm_start_transaction(tl, ffi.cast("stm_jmpbuf_t *", -1))
        self.running_transaction.add(n)

    def switch(self, thread_num):
        assert thread_num != self.current_thread
        self.current_thread = thread_num
        if lib._stm_is_in_transaction():
            stm_start_safe_point()
        lib._stm_restore_local_state(thread_num)
        if lib._stm_is_in_transaction():
            stm_stop_safe_point() # can raise Conflict
