import py
from support import *


def setup_function(f):
    lib.stm_clear_between_tests()
    lib.stm_initialize_tests(getattr(f, 'max_aborts', 0))

def teardown_function(_):
    lib.stm_finalize()


def test_nursery_alloc():
    for i in range(20):
        p = nalloc(HDR)
        check_not_free(p)

def test_stm_roots():
    p1 = nalloc(HDR)
    p2 = nalloc(HDR)
    p3 = nalloc(HDR)
    seen = set()
    for i in range(20):
        lib.stm_push_root(p1)
        lib.stm_push_root(p3)
        p = nalloc(HDR)
        check_not_free(p)
        seen.add(p)
        p3 = lib.stm_pop_root()
        p1 = lib.stm_pop_root()
        check_not_free(p1)
        check_not_free(p3)
    assert p2 in seen    # the pointer location was reused

def test_nursery_follows():
    p1 = nalloc_refs(1)
    p2 = nalloc_refs(1)
    rawsetptr(p1, 0, p2)
    lib.stm_push_root(p1)
    minor_collect()
    check_nursery_free(p1)
    check_nursery_free(p2)
    p1b = lib.stm_pop_root()
    p2b = rawgetptr(p1b, 0)
    assert rawgetptr(p2b, 0) == ffi.NULL

def test_free_nursery_at_thread_end():
    p1 = nalloc(HDR)
    tid = p1.h_tid
    lib.stm_finalize()
    check_free(p1)
    lib.stm_initialize_tests(0)

def test_local_copy_out_of_nursery():
    p1 = oalloc(HDR + WORD); make_global(p1)
    lib.rawsetlong(p1, 0, 420063)
    assert not lib.in_nursery(p1)
    assert p1.h_revision != lib.get_local_revision()
    #
    p2 = lib.stm_write_barrier(p1)
    assert lib.rawgetlong(p2, 0) == 420063
    lib.rawsetlong(p2, 0, -91467)
    assert lib.in_nursery(p2)
    assert p2.h_revision == lib.get_local_revision()
    #
    lib.stm_push_root(p1)
    minor_collect()
    p1b = lib.stm_pop_root()
    assert p1b == p1
    assert lib.rawgetlong(p1b, 0) == 420063
    #
    p3 = lib.stm_read_barrier(p1)
    assert not lib.in_nursery(p3) and p3 != p2
    assert lib.rawgetlong(p3, 0) == -91467

def test_outer2inner():
    p1 = oalloc_refs(1)
    p3 = nalloc(HDR)
    p2 = lib.stm_write_barrier(p1)
    assert p2 == p1
    lib.rawsetptr(p2, 0, p3)
    #
    lib.stm_push_root(p1)
    minor_collect()
    check_nursery_free(p3)
    p1b = lib.stm_pop_root()
    assert p1b == p1
    p3b = lib.getptr(p1b, 0)
    assert p3b != p3
    check_not_free(p3b)

def test_outer2inner_after_transaction_end():
    p1 = oalloc(HDR + WORD); make_global(p1)
    lib.rawsetlong(p1, 0, 420063)
    p2 = lib.stm_write_barrier(p1)
    lib.rawsetlong(p2, 0, -91467)
    assert lib.in_nursery(p2)
    lib.stm_push_root(p1)
    print "committing..."
    transaction_break()
    print "done"

    # first check that the situation is still the same in the next transaction
    p1b = lib.stm_pop_root()
    assert p1b == p1
    p2b = lib.stm_read_barrier(p1b)
    assert lib.in_nursery(p2b)
    check_not_free(p2b)
    lib.stm_push_root(p1b)
    print 'ok'

    # then do a minor collection
    minor_collect()
    p1b = lib.stm_pop_root()
    assert p1b == p1
    # check that the link p1 -> p2 was kept alive by moving p2 outside
    p2b = lib.stm_read_barrier(p1b)
    assert not lib.in_nursery(p2b)
    check_not_free(p2b)

def test_minor_collection_at_thread_end():
    p1 = oalloc_refs(1); make_global(p1)
    p2 = nalloc(HDR)
    setptr(p1, 0, p2)
    lib.stm_finalize()
    lib.stm_initialize_tests(0)
    p1b = getptr(p1, 0)
    assert p1b != p1
    assert not lib.in_nursery(p1b)
    check_not_free(p1b)

def test_prebuilt_keeps_alive():
    p0 = palloc_refs(1)
    p1 = nalloc(HDR)
    lib.setptr(p0, 0, p1)
    minor_collect()
    check_nursery_free(p1)
    check_prebuilt(p0)
    p2 = lib.getptr(p0, 0)
    assert not lib.in_nursery(p2)
    check_not_free(p2)

def test_prebuilt_keeps_alive_at_thread_end():
    p0 = palloc_refs(1)
    p1 = nalloc(HDR)
    lib.setptr(p0, 0, p1)
    lib.stm_finalize()
    lib.stm_initialize_tests(0)
    check_prebuilt(p0)
    p2 = lib.getptr(p0, 0)
    check_not_free(p2)

def test_new_version():
    p1 = oalloc(HDR)
    assert lib.stm_write_barrier(p1) == p1
    lib.stm_push_root(p1)
    transaction_break()
    p1b = lib.stm_pop_root()
    assert p1b == p1
    p2 = lib.stm_write_barrier(p1)
    assert p2 != p1
    assert lib.in_nursery(p2)
    check_not_free(p1)
    check_not_free(p2)
    lib.stm_push_root(p1)
    minor_collect()
    p1b = lib.stm_pop_root()
    assert p1b == p1
    check_not_free(p1)
    check_nursery_free(p2)
    p2 = lib.stm_read_barrier(p1)
    p3 = lib.stm_write_barrier(p1)
    assert p3 == p2 != p1
    assert not lib.in_nursery(p2)

def test_prebuilt_version():
    p1 = lib.pseudoprebuilt(HDR, 42 + HDR)
    p2 = lib.stm_write_barrier(p1)
    assert p2 != p1
    check_prebuilt(p1)
    check_not_free(p2)
    minor_collect()
    check_prebuilt(p1)
    check_nursery_free(p2)
    p2 = lib.stm_read_barrier(p1)
    p3 = lib.stm_write_barrier(p1)
    assert p3 == p2 != p1
    assert not lib.in_nursery(p2)

def test_two_transactions_in_nursery():
    p1 = lib.pseudoprebuilt(HDR + WORD, 42 + HDR + WORD)
    lib.rawsetlong(p1, 0, 5111111)
    p2 = lib.stm_write_barrier(p1)
    assert p2 != p1
    lib.rawsetlong(p2, 0, 5222222)
    transaction_break()
    check_prebuilt(p1)
    check_not_free(p2)
    p3 = lib.stm_write_barrier(p2)
    assert p3 != p2 and p3 != p1
    lib.rawsetlong(p3, 0, 5333333)
    minor_collect()
    check_prebuilt(p1)
    check_nursery_free(p2)
    check_nursery_free(p3)
    p3b = lib.stm_read_barrier(p1)
    check_not_free(p3b)
    assert lib.rawgetlong(p3b, 0) == 5333333
    assert p3b == lib.stm_write_barrier(p1)

def test_write_barrier_cannot_collect():
    p1l = []
    p2l = []
    for i in range(17):
        p1 = oalloc(HDR); make_global(p1)
        p2 = lib.stm_write_barrier(p1)     # must not collect!
        p1l.append(p1)
        p2l.append(p2)
    for p1 in p1l:
        check_not_free(p1)
    for p2 in p2l:
        check_not_free(p2)

def test_list_of_read_objects():
    def f1(r):
        p1 = palloc_refs(1)
        p2 = lib.stm_write_barrier(p1)
        def cb(_):
            assert lib.stm_read_barrier(p1) == p2
            minor_collect()   # move p2, but p2 is in list_of_read_objects
            check_prebuilt(p1)
            p2b = lib.stm_read_barrier(p1)
            assert p2b != p2     # has been moved
            check_not_free(p2b)
        perform_transaction(cb)
    run_parallel(f1, f1, f1)

def test_many_versions():
    p1 = nalloc(HDR)
    for i in range(10):
        lib.stm_commit_transaction()
        lib.stm_begin_inevitable_transaction()
        p1 = lib.stm_write_barrier(p1)

def test_access_foreign_nursery():
    pg = palloc(HDR)
    seen = []
    def f1(r):
        p1 = lib.stm_write_barrier(pg)
        seen.append(p1)
        assert lib.in_nursery(p1)
        r.set(2)
        r.wait(3)
        p3 = lib.stm_read_barrier(pg)
        assert not lib.in_nursery(p3)
    def f2(r):
        r.wait(2)
        p2 = lib.stm_read_barrier(pg)
        assert not lib.in_nursery(p2)
        r.set(3)
    run_parallel(f1, f2)

def test_access_foreign_nursery_with_private_copy_1():
    # this version should not cause conflicts
    pg = palloc(HDR + WORD)
    lib.rawsetlong(pg, 0, 420063)
    seen = []
    def f1(r):
        p1 = lib.stm_write_barrier(pg)
        assert lib.in_nursery(p1)
        lib.rawsetlong(p1, 0, 9387987)
        def cb(c):
            assert c == 0
            p4 = lib.stm_write_barrier(p1)
            assert lib.in_nursery(p4)
            assert p4 != p1 and p4 != pg
            assert lib.rawgetlong(p4, 0) == 9387987
            print "changing p4=%r to contain -6666" % (p4,)
            lib.rawsetlong(p4, 0, -6666)
            r.wait_while_in_parallel()
            assert seen == ["ok"]
        perform_transaction(cb)
    def f2(r):
        def cb(c):
            assert c == 0
            r.enter_in_parallel()
            p2 = lib.stm_read_barrier(pg)
            assert not lib.in_nursery(p2)
            assert lib.rawgetlong(p2, 0) == 9387987
        perform_transaction(cb)
        seen.append("ok")
        r.leave_in_parallel()
    run_parallel(f1, f2)
    assert lib.getlong(pg, 0) == -6666

def test_access_foreign_nursery_use_stubs():
    pg = palloc_refs(1)
    def f1(r):
        q1 = nalloc(HDR + WORD)
        lib.setlong(q1, 0, 424242)
        p1 = lib.stm_write_barrier(pg)
        lib.setptr(p1, 0, q1)
        assert lib.in_nursery(p1)
        assert lib.in_nursery(q1)
        r.set(2)
        r.wait(3)
        p3 = lib.stm_read_barrier(pg)
        assert not lib.in_nursery(p3)
    def f2(r):
        r.wait(2)
        p2 = lib.stm_read_barrier(pg)
        assert not lib.in_nursery(p2)
        assert p2
        q2 = lib.getptr(p2, 0)
        assert lib.getlong(q2, 0) == 424242
        r.set(3)
    run_parallel(f1, f2)

def test_stubs_are_public_to_young_collect():
    pg = palloc_refs(2)
    #
    p1 = lib.stm_write_barrier(pg)
    assert lib.in_nursery(p1)
    lib.stm_push_root(p1)
    minor_collect()
    p1 = lib.stm_pop_root()
    assert not lib.in_nursery(p1)
    q1 = nalloc(HDR + WORD)
    lib.setlong(q1, 0, 424242)
    lib.setptr(p1, 0, q1)
    lib.setptr(p1, 1, q1)
    assert not lib.in_nursery(p1)
    assert lib.in_nursery(q1)
    #
    lib.stm_commit_transaction()
    lib.stm_begin_inevitable_transaction()
    #
    assert not lib.in_nursery(p1)   # public
    assert lib.in_nursery(q1)       # protected
    lib.stm_push_root(p1)
    minor_collect()
    p1b = lib.stm_pop_root()
    assert p1b == p1
    q1b = lib.getptr(p1, 0)
    assert q1b == lib.getptr(p1, 1)
    assert not lib.in_nursery(q1b)
    assert lib.getlong(q1b, 0) == 424242

def test_stubs_are_public_to_young_steal():
    pg = palloc_refs(1)
    seen = []
    def f1(r):
        q1 = nalloc(HDR + WORD)
        lib.setlong(q1, 0, 424242)
        p1 = lib.stm_write_barrier(pg)
        lib.setptr(p1, 0, q1)
        assert lib.in_nursery(p1)
        assert lib.in_nursery(q1)
        r.set(2)
        r.wait(3)
        minor_collect()
        p4 = lib.getptr(pg, 0)
        assert not lib.in_nursery(p4)     # the stub
        assert lib.getlong(p4, 0) == 424242
    def f2(r):
        r.wait(2)
        p2 = lib.stm_read_barrier(pg)    # foreign nursery -> old
        assert not lib.in_nursery(p2)
        assert p2
        # don't read getptr(p2, 0) here, so that it remains as a stub
        r.set(3)
    run_parallel(f1, f2)
