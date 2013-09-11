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
    c = lib.stm_inspect_abort_info()
    assert not c
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
            c = lib.stm_inspect_abort_info()
            assert not c

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
    def run(retry_counter):
        if retry_counter == 0:
            p = nalloc_refs(2)
            lib.stm_push_root(p)
            q = nalloc(HDR + 2 * WORD)
            p = lib.stm_pop_root()
            lib.setptr(p, 1, q)
            lib.setlong(q, 0, 3)
            word = "ABC" + "\xFF" * (WORD - 3)
            lib.setlong(q, 1, struct.unpack("l", word)[0])
            lib.stm_abort_info_push(p, fo1)
            possibly_collect()
            abort_and_retry()
        else:
            possibly_collect()
            c = lib.stm_inspect_abort_info()
            assert c
            assert ffi.string(c).endswith("e3:ABCe")
    #
    def no_collect():
        pass
    for possibly_collect in [no_collect, minor_collect, major_collect]:
        print '-'*79
        print 'running with', possibly_collect
        perform_transaction(run)

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

def test_clear_original_on_id_copy():
    p1 = nalloc(HDR)
    pid = lib.stm_id(p1)
    lib.stm_push_root(p1)
    minor_collect()
    p1o = lib.stm_pop_root()

    assert p1o == ffi.cast("gcptr", pid)
    assert follow_original(p1o) == ffi.NULL
    
def test_clear_original_on_priv_from_prot_abort():
    p = oalloc(HDR+WORD)
    
    def cb(c):
        if c == 0:
            pw = lib.stm_write_barrier(p)
            abort_and_retry()
    lib.stm_push_root(p)
    perform_transaction(cb)
    p = lib.stm_pop_root()
    assert follow_original(p) == ffi.NULL

def test_set_backups_original_on_move_to_id_copy():
    p1 = nalloc(HDR+WORD)
    lib.stm_commit_transaction()
    lib.stm_begin_inevitable_transaction()
    assert classify(p1) == 'protected'

    pw = lib.stm_write_barrier(p1)
    assert classify(pw) == 'private_from_protected'
    assert pw == p1
    
    lib.stm_push_root(pw)
    # make pw old
    minor_collect()
    p1o = lib.stm_pop_root()

    # Backup has updated h_original:
    assert classify(p1o) == 'private_from_protected'
    B = follow_revision(p1o)
    assert follow_original(B) == p1o
    

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

def test_clear_on_abort():
    p = ffi.new("char[]", "hello")
    lib.stm_clear_on_abort(p, 2)
    #
    @perform_transaction
    def run(retry_counter):
        if retry_counter == 0:
            assert ffi.string(p) == "hello"
            abort_and_retry()
        else:
            assert p[0] == '\0'
            assert p[1] == '\0'
            assert p[2] == 'l'
            assert p[3] == 'l'
            assert p[4] == 'o'

def test_call_on_abort():
    p0 = ffi.new("char[]", "aaa")
    p1 = ffi.new("char[]", "hello")
    p2 = ffi.new("char[]", "removed")
    p3 = ffi.new("char[]", "world")
    #
    @ffi.callback("void(void *)")
    def clear_me(p):
        p = ffi.cast("char *", p)
        p[0] = chr(ord(p[0]) + 1)
    #
    lib.stm_call_on_abort(p0, clear_me)
    # the registered callbacks are removed on
    # successful commit
    lib.stm_commit_transaction()
    lib.stm_begin_inevitable_transaction()
    #
    @perform_transaction
    def run(retry_counter):
        if retry_counter == 0:
            lib.stm_call_on_abort(p1, clear_me)
            lib.stm_call_on_abort(p2, clear_me)
            lib.stm_call_on_abort(p3 + 1, clear_me)
            lib.stm_call_on_abort(p2, ffi.NULL)
        #
        assert ffi.string(p0) == "aaa"
        assert ffi.string(p2) == "removed"
        if retry_counter == 0:
            assert ffi.string(p1) == "hello"
            assert ffi.string(p3) == "world"
            abort_and_retry()
        else:
            assert ffi.string(p1) == "iello"
            assert ffi.string(p3) == "wprld"
            if retry_counter == 1:
                # the registered callbacks are removed
                # on abort
                abort_and_retry()
