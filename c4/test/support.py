import os, cffi, thread, sys

# ----------

parent_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

header_files = [os.path.join(parent_dir, _n) for _n in
                "et.h lists.h steal.h nursery.h gcpage.h "
                "stmsync.h dbgmem.h fprintcolor.h "
                "stmgc.h stmimpl.h atomic_ops.h".split()]
source_files = [os.path.join(parent_dir, _n) for _n in
                "et.c lists.c steal.c nursery.c gcpage.c "
                "stmsync.c dbgmem.c fprintcolor.c".split()]

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

ffi.cdef('''
    typedef intptr_t revision_t;
    typedef struct stm_object_s {
        revision_t h_tid;
        revision_t h_revision;
    } *gcptr;

    int gettid(gcptr);
    void settid(gcptr, int);

    #define PREBUILT_FLAGS         ...
    #define PREBUILT_REVISION      ...

    gcptr stm_allocate(size_t size, unsigned int tid);
    void stm_push_root(gcptr);
    gcptr stm_pop_root(void);
    void stm_set_max_aborts(int max_aborts);
    void stm_finalize(void);
    int stm_in_transaction(void);
    gcptr stm_read_barrier(gcptr);
    gcptr stm_write_barrier(gcptr);
    void stm_perform_transaction(gcptr arg, int (*callback)(gcptr, int));
    void stm_commit_transaction(void);
    void stm_begin_inevitable_transaction(void);
    void stm_set_transaction_length(long length_max);

    /* extra non-public code */
    void *stm_malloc(size_t size);
    gcptr stmgcpage_malloc(size_t size);
    void stmgcpage_free(gcptr obj);
    long stmgcpage_count(int quantity);
    void stmgcpage_possibly_major_collect(int);
    revision_t stm_global_cur_time(void);
    //void stmgcpage_add_prebuilt_root(gcptr);
    void stm_clear_between_tests(void);
    void stmgc_minor_collect(void);
    gcptr _stm_nonrecord_barrier(gcptr);
    int _stm_is_private(gcptr);
    void stm_start_sharedlock(void);
    void stm_stop_sharedlock(void);
    void AbortTransaction(int);
    gcptr stm_get_private_from_protected(long index);
    gcptr stm_get_read_obj(long index);
    void *STUB_THREAD(gcptr);
    void stm_clear_read_cache(void);
    int in_nursery(gcptr);

    gcptr getptr(gcptr, long);
    void setptr(gcptr, long, gcptr);
    gcptr rawgetptr(gcptr, long);
    void rawsetptr(gcptr, long, gcptr);

    long getlong(gcptr, long);
    void setlong(gcptr, long, long);
    long rawgetlong(gcptr, long);
    void rawsetlong(gcptr, long, long);

    gcptr pseudoprebuilt(size_t size, int tid);
    revision_t get_private_rev_num(void);
    revision_t get_start_time(void);
    void *my_stub_thread(void);

    int _stm_can_access_memory(char *);
    void stm_initialize_and_set_max_abort(int max_aborts);
    void stm_initialize_tests(int max_aborts);

    /* some constants normally private that are useful in the tests */
    #define WORD                     ...
    #define GC_PAGE_SIZE             ...
    #define STUB_BLOCK_SIZE          ...
    #define GCFLAG_OLD               ...
    #define GCFLAG_VISITED           ...
    #define GCFLAG_PUBLIC            ...
    #define GCFLAG_PREBUILT_ORIGINAL ...
    #define GCFLAG_BACKUP_COPY       ...
    #define GCFLAG_PUBLIC_TO_PRIVATE ...
    #define GCFLAG_WRITE_BARRIER     ...
    #define GCFLAG_NURSERY_MOVED     ...
    #define GCFLAG_STUB              ...
    #define GCFLAG_PRIVATE_FROM_PROTECTED  ...
    #define ABRT_MANUAL              ...
    typedef struct { ...; } page_header_t;
''')

lib = ffi.verify(r'''
    #include "stmgc.h"
    #include "stmimpl.h"

    extern revision_t stm_global_cur_time(void);
    //extern void stmgcpage_add_prebuilt_root(gcptr);
    extern revision_t get_private_rev_num(void);

    int gettid(gcptr obj)
    {
        int result = stm_get_tid(obj);
        assert(0 <= result && result < 521);
        return result;
    }

    void settid(gcptr obj, int newtid)
    {
        stm_set_tid(obj, newtid);
    }

    gcptr rawgetptr(gcptr obj, long index)
    {
        assert(gettid(obj) > 421 + index);
        return ((gcptr *)(obj + 1))[index];
    }

    void rawsetptr(gcptr obj, long index, gcptr newvalue)
    {
        fprintf(stderr, "%p->[%ld] = %p\n", obj, index, newvalue);
        assert(gettid(obj) > 421 + index);
        ((gcptr *)(obj + 1))[index] = newvalue;
    }

    gcptr getptr(gcptr obj, long index)
    {
        obj = stm_read_barrier(obj);
        return rawgetptr(obj, index);
    }

    void setptr(gcptr obj, long index, gcptr newvalue)
    {
        obj = stm_write_barrier(obj);
        fprintf(stderr, "setptr: write_barrier: %p, writing [%ld] = %p\n",
                obj, index, newvalue);
        rawsetptr(obj, index, newvalue);
    }

    long rawgetlong(gcptr obj, long index)
    {
        assert(stmcb_size(obj) >= sizeof(gcptr *) + (index+1)*sizeof(void *));
        return (long)((void **)(obj + 1))[index];
    }

    void rawsetlong(gcptr obj, long index, long newvalue)
    {
        assert(stmcb_size(obj) >= sizeof(gcptr *) + (index+1)*sizeof(void *));
        ((void **)(obj + 1))[index] = (void *)newvalue;
    }

    long getlong(gcptr obj, long index)
    {
        obj = stm_read_barrier(obj);
        return rawgetlong(obj, index);
    }

    void setlong(gcptr obj, long index, long newvalue)
    {
        obj = stm_write_barrier(obj);
        rawsetlong(obj, index, newvalue);
    }

    int in_nursery(gcptr obj)
    {
        struct tx_descriptor *d = thread_descriptor;
        int result1 = (d->nursery_base <= (char*)obj &&
                       ((char*)obj) < d->nursery_end);
        if (obj->h_tid & GCFLAG_OLD) {
            assert(result1 == 0);
        }
        else {
            /* this assert() also fails if "obj" is in another nursery than
               the one of the current thread.  This is ok, because we
               should not see such pointers. */
            assert(result1 == 1);
        }
        return result1;
    }

    gcptr pseudoprebuilt(size_t size, int tid)
    {
        gcptr x = calloc(1, size);
        x->h_tid = PREBUILT_FLAGS | tid;
        x->h_revision = PREBUILT_REVISION;
        return x;
    }

    revision_t get_start_time(void)
    {
        return thread_descriptor->start_time;
    }

    void *my_stub_thread(void)
    {
        return (void *)thread_descriptor->public_descriptor;
    }

    void stm_initialize_and_set_max_abort(int max_aborts)
    {
        stm_initialize();
        stm_set_max_aborts(max_aborts);
    }

    void stm_initialize_tests(int max_aborts)
    {
        _stm_test_forget_previous_state();
        stm_initialize_and_set_max_abort(max_aborts);
    }

    size_t stmcb_size(gcptr obj)
    {
        if (gettid(obj) < 421) {
            /* basic case: tid equals 42 plus the size of the object */
            assert(gettid(obj) >= 42 + sizeof(struct stm_object_s));
            return gettid(obj) - 42;
        }
        else {
            int nrefs = gettid(obj) - 421;
            assert(nrefs < 100);
            return sizeof(*obj) + nrefs * sizeof(gcptr);
        }
    }

    void stmcb_trace(gcptr obj, void visit(gcptr *))
    {
        int i;
        if (gettid(obj) < 421) {
            /* basic case: no references */
            return;
        }
        for (i=0; i < gettid(obj) - 421; i++) {
            gcptr *ref = ((gcptr *)(obj + 1)) + i;
            visit(ref);
        }
    }
'''.lstrip(),    include_dirs=[parent_dir],
                 undef_macros=['NDEBUG'],
                 define_macros=[('GC_NURSERY', '(16 * sizeof(void *))'),
                                ('_GC_DEBUG', '2'),
                                ('GC_PAGE_SIZE', '1000'),
                                ('GC_MIN', '200000'),
                                ('GC_EXPAND', '90000'),
                                ('_GC_ON_CPYTHON', '1'),
                                ],
                 libraries=['rt'],
                 sources=source_files,
                 extra_compile_args=['-g', '-O0'])

HDR = ffi.sizeof("struct stm_object_s")
WORD = lib.WORD
PAGE_ROOM = lib.GC_PAGE_SIZE - ffi.sizeof("page_header_t")
for name in lib.__dict__:
    if name.startswith('GCFLAG_') or name.startswith('PREBUILT_'):
        globals()[name] = getattr(lib, name)

def distance(p1, p2):
    return ffi.cast("char *", p2) - ffi.cast("char *", p1)

def count_global_pages():
    return lib.stmgcpage_count(0)

def count_pages():
    return lib.stmgcpage_count(1)

# ----------

class Interrupted(Exception):
    pass

def acquire_lock(lock):
    lib.stm_commit_transaction()
    lock.acquire()
    lib.stm_begin_inevitable_transaction()

class run_parallel(object):
    def __init__(self, *fns, **kwds):
        self.in_transaction = lib.stm_in_transaction()
        if self.in_transaction:
            lib.stm_commit_transaction()
        self.max_aborts = kwds.pop('max_aborts', 0)
        assert not kwds
        self.step = 0
        self.steplocks = {0: thread.allocate_lock()}
        self.settinglock = thread.allocate_lock()
        self.parallel_locks = (thread.allocate_lock(), thread.allocate_lock())
        self.parallel_locks[0].acquire()
        self.resulting_exception = None
        self.start_locks = []
        for fn in fns:
            self.start_locks.append(thread.allocate_lock())
            self.start_locks[-1].acquire()
        locks = []
        for i, fn in enumerate(fns):
            lck = thread.allocate_lock()
            lck.acquire()
            thread.start_new_thread(self.run, (fn, lck, i))
            locks.append(lck)
        for lck in locks:
            lck.acquire()
        if self.in_transaction:
            lib.stm_begin_inevitable_transaction()
        if self.resulting_exception is not None:
            exc, val, tb = self.resulting_exception
            raise exc, val, tb

    def run(self, fn, lck, i):
        try:
            try:
                lib.stm_initialize_and_set_max_abort(self.max_aborts)
                try:
                    # wait here until all threads reach this point
                    self.start_locks[i].release()
                    for _lck1 in self.start_locks:
                        acquire_lock(_lck1)
                        _lck1.release()
                    #
                    set_value = fn(self)
                finally:
                    lib.stm_finalize()
                if set_value is not None:
                    self.set_afterwards(set_value)
            except Interrupted:
                pass
            except:
                self.resulting_exception = sys.exc_info()
                self.set_interrupted()
        finally:
            lck.release()

    def _get_lock(self, num):
        # Get the lock corresponding to the step 'num'.  At any point of
        # time, all these locks are acquired, apart from possibly the
        # one corresponding to 'self.step'.
        try:
            return self.steplocks[num]
        except KeyError:
            lck = thread.allocate_lock()
            lck.acquire()
            return self.steplocks.setdefault(num, lck)

    def wait(self, num):
        # Wait until 'self.step == num'.
        print 'wait(%d)' % num
        while self.step != num:
            if self.step is Interrupted:
                raise Interrupted
            lck = self._get_lock(num)
            acquire_lock(lck)
            lck.release()
        print 'wait(%d) -> ok' % num

    def set(self, num):
        # Set the value of 'self.step' to 'num'.
        with self.settinglock:
            acquire_lock(self._get_lock(self.step))
            self.step = num
            print 'set(%d)' % num
            self._get_lock(num).release()

    def set_afterwards(self, num):
        # Set the value of 'self.step' to 'num'.
        with self.settinglock:
            self._get_lock(self.step).acquire()
            self.step = num
            print 'set_afterwards(%d)' % num
            self._get_lock(num).release()

    def set_interrupted(self):
        with self.settinglock:
            self.step = Interrupted
            for value in self.steplocks.values():
                try:
                    value.release()
                except thread.error:
                    pass

    def wait_while_in_parallel(self):
        # parallel_locks[0] is acquired, parallel_locks[1] is released
        res = self.parallel_locks[1].acquire(False)
        assert res
        # parallel_locks[0] is acquired, parallel_locks[1] is acquired
        print 'wait_while_in_parallel enter'
        self.parallel_locks[0].release()
        self.parallel_locks[1].acquire()
        print 'wait_while_in_parallel leave'
        # parallel_locks[0] is acquired, parallel_locks[1] is acquired
        self.parallel_locks[1].release()
        res = self.parallel_locks[0].acquire(False)
        assert not res
        # parallel_locks[0] is acquired, parallel_locks[1] is released

    def enter_in_parallel(self):
        print 'enter_in_parallel: waiting...'
        # wait for parallel_locks[0]
        self.parallel_locks[0].acquire()
        print 'enter_in_parallel'

    def leave_in_parallel(self):
        print 'leave_in_parallel'
        self.parallel_locks[1].release()

# ____________________________________________________________

def oalloc(size):
    "Allocate an 'old' protected object, outside any nursery"
    p = lib.stmgcpage_malloc(size)
    p.h_tid = GCFLAG_OLD | GCFLAG_WRITE_BARRIER
    p.h_revision = -sys.maxint
    lib.settid(p, 42 + size)
    return p

def oalloc_refs(nrefs):
    """Allocate an 'old' protected object, outside any nursery,
    with nrefs pointers"""
    p = lib.stmgcpage_malloc(HDR + WORD * nrefs)
    p.h_tid = GCFLAG_OLD | GCFLAG_WRITE_BARRIER
    p.h_revision = -sys.maxint
    lib.settid(p, 421 + nrefs)
    for i in range(nrefs):
        rawsetptr(p, i, ffi.NULL)
    return p

ofree = lib.stmgcpage_free

def nalloc(size):
    "Allocate a fresh object from the nursery"
    p = lib.stm_allocate(size, 42 + size)
    assert p.h_tid == 42 + size     # no GC flags
    assert p.h_revision == lib.get_private_rev_num()
    return p

def nalloc_refs(nrefs):
    "Allocate a fresh object from the nursery, with nrefs pointers"
    p = lib.stm_allocate(HDR + WORD * nrefs, 421 + nrefs)
    assert p.h_revision == lib.get_private_rev_num()
    for i in range(nrefs):
        assert rawgetptr(p, i) == ffi.NULL   # must already be zero-filled
    return p

def palloc(size):
    "Get a ``prebuilt'' object."
    p = lib.pseudoprebuilt(size, 42 + size)
    assert p.h_revision == 1
    return p

def palloc_refs(nrefs):
    "Get a ``prebuilt'' object with nrefs pointers."
    p = lib.pseudoprebuilt(HDR + WORD * nrefs, 421 + nrefs)
    return p

gettid = lib.gettid
setptr = lib.setptr
getptr = lib.getptr
rawsetptr = lib.rawsetptr
rawgetptr = lib.rawgetptr

def major_collect():
    lib.stmgcpage_possibly_major_collect(1)

def minor_collect():
    lib.stmgc_minor_collect()

def is_stub(p):
    return p.h_tid & GCFLAG_STUB

def check_not_free(p):
    assert 42 < (p.h_tid & 0xFFFF) < 521

def check_nursery_free(p):
    #assert p.h_tid == p.h_revision == 0
    assert not lib._stm_can_access_memory(p)

def check_inaccessible(p):
    assert not lib._stm_can_access_memory(p)

def DEBUG_WORD(char):
    return int(ffi.cast("revision_t", char * 0x0101010101010101))

def check_free_old(p):
    assert not lib._stm_can_access_memory(p) or p.h_tid == DEBUG_WORD(0xDD)

def check_free_explicitly(p):
    assert not lib._stm_can_access_memory(p) or p.h_tid in (
        DEBUG_WORD(0x55), DEBUG_WORD(0xAA))

def check_prebuilt(p):
    assert 42 < (p.h_tid & 0xFFFF) < 521
    assert p.h_tid & GCFLAG_PREBUILT_ORIGINAL

def delegate(p1, p2):
    assert classify(p1) == "public"
    assert classify(p2) == "public"
    p1.h_revision = ffi.cast("revision_t", p2)
    p1.h_tid |= GCFLAG_PUBLIC_TO_PRIVATE
    if p1.h_tid & GCFLAG_PREBUILT_ORIGINAL:
        lib.stmgcpage_add_prebuilt_root(p1)

def make_public(p1):
    """Hack at an object returned by oalloc() to force it public."""
    assert classify(p1) == "protected"
    assert p1.h_tid & GCFLAG_OLD
    p1.h_tid |= GCFLAG_PUBLIC
    assert classify(p1) == "public"
    assert (p1.h_tid & GCFLAG_PUBLIC_TO_PRIVATE) == 0

def transaction_break():
    lib.stm_commit_transaction()
    lib.stm_begin_inevitable_transaction()

def perform_transaction(callback):
    @ffi.callback("int(gcptr, int)", error=0)
    def cb(_, retry_counter):
        del fine[:]
        try:
            loopback = callback(retry_counter)
        except Exception:
            if not sys.stdout.isatty():
                raise
            import pdb; pdb.post_mortem(sys.exc_info()[2])
            raise
        if loopback:
            return 1
        fine.append(True)
        return 0
    fine = []
    lib.stm_perform_transaction(ffi.NULL, cb)
    assert fine == [True]

def abort_and_retry():
    lib.AbortTransaction(lib.ABRT_MANUAL)

def classify(p):
    private_from_protected = (p.h_tid & GCFLAG_PRIVATE_FROM_PROTECTED) != 0
    private_other = p.h_revision == lib.get_private_rev_num()
    public  = (p.h_tid & GCFLAG_PUBLIC) != 0
    backup  = (p.h_tid & GCFLAG_BACKUP_COPY) != 0
    stub    = (p.h_tid & GCFLAG_STUB) != 0
    assert private_from_protected + private_other + public + backup <= 1
    assert (public, stub) != (False, True)
    if private_from_protected:
        return "private_from_protected"
    if private_other:
        return "private"
    if public:
        if stub:
            assert not lib.in_nursery(p)
            return "stub"
        else:
            # public objects usually never live in the nursery, but
            # if stealing makes one, it has GCFLAG_NURSERY_MOVED.
            if lib.in_nursery(p):
                assert p.h_tid & GCFLAG_NURSERY_MOVED
                assert not (p.h_revision & 1)   # "is a pointer"
            return "public"
    if backup:
        return "backup"
    else:
        return "protected"

def _get_full_list(getter):
    result = []
    index = 0
    while 1:
        p = getter(index)
        if p == ffi.NULL:
            break
        result.append(p)
        index += 1
    return result

def list_of_read_objects():
    return _get_full_list(lib.stm_get_read_obj)

def list_of_private_from_protected():
    return _get_full_list(lib.stm_get_private_from_protected)

stub_thread = lib.STUB_THREAD

def follow_revision(p):
    r = p.h_revision
    assert (r % 4) == 0
    return ffi.cast("gcptr", r)

nrb_protected = ffi.cast("gcptr", -1)
