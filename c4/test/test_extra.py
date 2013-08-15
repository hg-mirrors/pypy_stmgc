import py, sys, struct
from support import *


def setup_function(f):
    lib.stm_clear_between_tests()
    lib.stm_initialize_tests(getattr(f, 'max_aborts', 0))

def teardown_function(_):
    lib.stm_finalize()


def test_abort_info_stack():
    p = nalloc(HDR)
    q = nalloc(HDR)
    lib.stm_abort_info_push(p, ffi.cast("long *", 123))
    lib.stm_abort_info_push(q, ffi.cast("long *", 125))
    lib.stm_abort_info_pop(2)
    # no real test here

def test_inspect_abort_info_signed():
    fo1 = ffi.new("long[]", [-2, 1, HDR, -1, 0])
    #
    @perform_transaction
    def run(retry_counter):
        if retry_counter == 0:
            p = nalloc(HDR + WORD)
            lib.setlong(p, 0, -421289712)
            lib.stm_abort_info_push(p, fo1)
            abort_and_retry()
        else:
            c = lib.stm_inspect_abort_info()
            assert c
            assert ffi.string(c).endswith("eli-421289712eee")

def test_inspect_abort_info_nested_unsigned():
    fo1 = ffi.new("long[]", [-2, 2, HDR, 0])
    fo2 = ffi.new("long[]", [2, HDR + WORD, -1, 0])
    #
    @perform_transaction
    def run(retry_counter):
        if retry_counter == 0:
            p = nalloc(HDR + WORD)
            q = nalloc(HDR + 2 * WORD)
            lib.setlong(p, 0, sys.maxint)
            lib.setlong(q, 1, -1)
            lib.stm_abort_info_push(p, fo1)
            lib.stm_abort_info_push(q, fo2)
            abort_and_retry()
        else:
            c = lib.stm_inspect_abort_info()
            assert c
            assert ffi.string(c).endswith("eli%dei%deee" % (
                sys.maxint, sys.maxint * 2 + 1))

def test_inspect_abort_info_string():
    fo1 = ffi.new("long[]", [3, HDR + WORD, HDR, 0])
    #
    @perform_transaction
    def run(retry_counter):
        if retry_counter == 0:
            p = nalloc_refs(2)
            q = nalloc(HDR + 2 * WORD)
            lib.setptr(p, 1, q)
            lib.setlong(q, 0, 3)
            word = "ABC" + "\xFF" * (WORD - 3)
            lib.setlong(q, 1, struct.unpack("l", word)[0])
            lib.stm_abort_info_push(p, fo1)
            abort_and_retry()
        else:
            c = lib.stm_inspect_abort_info()
            assert c
            assert ffi.string(c).endswith("e3:ABCe")

def test_inspect_null():
    fo1 = ffi.new("long[]", [3, HDR, HDR + 1, 0])
    #
    @perform_transaction
    def run(retry_counter):
        if retry_counter == 0:
            p = nalloc_refs(1)
            lib.setptr(p, 0, ffi.NULL)    # default
            lib.stm_abort_info_push(p, fo1)
            abort_and_retry()
        else:
            c = lib.stm_inspect_abort_info()
            assert c
            assert ffi.string(c).endswith("e0:e")

def test_latest_version():
    fo1 = ffi.new("long[]", [1, HDR, 0])
    p = palloc(HDR + WORD)
    lib.rawsetlong(p, 0, -9827892)
    #
    @perform_transaction
    def run(retry_counter):
        if retry_counter == 0:
            lib.stm_abort_info_push(p, fo1)
            lib.setlong(p, 0, 424242)
            abort_and_retry()
        else:
            c = lib.stm_inspect_abort_info()
            assert c
            assert ffi.string(c).endswith("ei424242ee")

def test_pointer_equal():
    p1 = palloc(HDR + WORD)
    p2 = palloc(HDR + WORD)
    p3 = oalloc(HDR + WORD)
    p4 = nalloc(HDR + WORD)
    lib.stm_commit_transaction()
    lib.stm_begin_inevitable_transaction()
    p1b = lib.stm_write_barrier(p1)
    p2b = lib.stm_write_barrier(p2)
    p3b = lib.stm_write_barrier(p3)
    p4b = lib.stm_write_barrier(p4)
    #
    got = []
    for qa in [ffi.NULL, p1, p1b, p2, p2b, p3, p3b, p4, p4b]:
        for qb in [ffi.NULL, p1, p1b, p2, p2b, p3, p3b, p4, p4b]:
            got.append(lib.stm_pointer_equal(qa, qb))
    #
    assert got == [1, 0, 0, 0, 0, 0, 0, 0, 0,
                   0, 1, 1, 0, 0, 0, 0, 0, 0,
                   0, 1, 1, 0, 0, 0, 0, 0, 0,
                   0, 0, 0, 1, 1, 0, 0, 0, 0,
                   0, 0, 0, 1, 1, 0, 0, 0, 0,
                   0, 0, 0, 0, 0, 1, 1, 0, 0,
                   0, 0, 0, 0, 0, 1, 1, 0, 0,
                   0, 0, 0, 0, 0, 0, 0, 1, 1,
                   0, 0, 0, 0, 0, 0, 0, 1, 1]

def test_pointer_equal_prebuilt():
    p1 = palloc(HDR + WORD)
    p2 = palloc(HDR + WORD)
    p3 = oalloc(HDR + WORD)
    p4 = nalloc(HDR + WORD)
    lib.stm_commit_transaction()
    lib.stm_begin_inevitable_transaction()
    p1b = lib.stm_write_barrier(p1)
    p2b = lib.stm_write_barrier(p2)
    p3b = lib.stm_write_barrier(p3)
    p4b = lib.stm_write_barrier(p4)
    #
    got = []
    for qa in [ffi.NULL, p1, p1b, p2, p2b, p3, p3b, p4, p4b]:
        for qb in [p1, p2]:
            got.append(lib.stm_pointer_equal_prebuilt(qa, qb))
    #
    assert got == [0, 0,
                   1, 0,
                   1, 0,
                   0, 1,
                   0, 1,
                   0, 0,
                   0, 0,
                   0, 0,
                   0, 0]

def test_bug():
    p1 = nalloc(HDR)
    pid = lib.stm_id(p1)
    lib.stm_push_root(p1)
    minor_collect()
    p1o = lib.stm_pop_root()

    assert p1o == ffi.cast("gcptr", pid)
    assert follow_original(p1o) == ffi.NULL
    
def test_bug2():
    p = oalloc(HDR+WORD)
    
    def cb(c):
        if c == 0:
            pw = lib.stm_write_barrier(p)
            abort_and_retry()
    lib.stm_push_root(p)
    perform_transaction(cb)
    p = lib.stm_pop_root()
    assert follow_original(p) == ffi.NULL

    

def test_allocate_public_integer_address():
    p1 = palloc(HDR)
    p2 = oalloc(HDR)
    p3 = nalloc(HDR)
    lib.stm_push_root(p3)
    p3p = lib.stm_allocate_public_integer_address(p3)
    p1p = lib.stm_allocate_public_integer_address(p1)
    p2p = lib.stm_allocate_public_integer_address(p2)

    # p3 stub points to p3o:
    p3o = lib.stm_pop_root()
    p3po = ffi.cast("gcptr", p3p)
    assert ffi.cast("gcptr", p3po.h_revision - 2) == p3o

    # we have stubs here:
    assert ffi.cast("gcptr", p1p).h_tid & GCFLAG_PUBLIC
    assert classify(ffi.cast("gcptr", p1p)) == 'stub'
    assert classify(ffi.cast("gcptr", p2p)) == 'stub'
    assert classify(ffi.cast("gcptr", p3p)) == 'stub'

    major_collect()

    # kept alive through stubs:
    check_not_free(p3o)
    check_not_free(p2)

    check_not_free(ffi.cast("gcptr", p1p))
    check_not_free(ffi.cast("gcptr", p2p))
    check_not_free(ffi.cast("gcptr", p3p))

    lib.stm_unregister_integer_address(p1p)
    lib.stm_unregister_integer_address(p2p)
    lib.stm_unregister_integer_address(p3p)

    major_collect()
    major_collect()
    
    check_free_old(p3o)
    check_free_old(p2)
    
    
