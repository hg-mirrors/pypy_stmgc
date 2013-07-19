import py
from support import *


COLLECT_MOVES_NURSERY = True


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
    if not COLLECT_MOVES_NURSERY:
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
    lib.stm_finalize()
    check_inaccessible(p1)
    lib.stm_initialize_tests(0)

def test_local_copy_out_of_nursery():
    p1 = palloc(HDR + WORD)
    lib.rawsetlong(p1, 0, 420063)
    assert not lib.in_nursery(p1)
    assert p1.h_revision != lib.get_private_rev_num()
    #
    p2 = lib.stm_write_barrier(p1)
    assert lib.rawgetlong(p2, 0) == 420063
    lib.rawsetlong(p2, 0, -91467)
    assert lib.in_nursery(p2)
    assert p2.h_revision == lib.get_private_rev_num()
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

def test_outer2inner():   # test mark_private_old_pointing_to_young()
    p1 = nalloc_refs(1)
    lib.stm_push_root(p1)
    minor_collect()
    check_nursery_free(p1)
    p1 = lib.stm_pop_root()
    assert classify(p1) == "private"
    p2 = nalloc(HDR + WORD)
    lib.setlong(p2, 0, 8972981)
    lib.setptr(p1, 0, p2)
    #
    lib.stm_push_root(p1)
    minor_collect()
    p1b = lib.stm_pop_root()
    assert p1b == p1
    check_nursery_free(p2)
    p2b = lib.getptr(p1b, 0)
    assert p2b != p2
    check_not_free(p2b)
    assert lib.getlong(p2b, 0) == 8972981

def test_outer2inner_after_transaction_end():
    p1 = palloc(HDR + WORD)
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
    assert classify(p1b) == "public"
    p2b = lib.stm_read_barrier(p1b)
    assert lib.in_nursery(p2b)
    assert p2b == p2
    assert classify(p2) == "protected"
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
    p1 = palloc_refs(1)
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

def test_old_protected_stay_alive():
    p0 = oalloc(HDR + WORD)
    assert classify(p0) == "protected"
    lib.rawsetlong(p0, 0, 81211)
    lib.stm_push_root(p0)
    minor_collect()
    p0b = lib.stm_pop_root()
    assert p0b == p0
    assert classify(p0) == "protected"
    assert lib.rawgetlong(p0, 0) == 81211

def test_old_private_from_protected():
    p0 = oalloc(HDR + WORD)
    assert classify(p0) == "protected"
    lib.setlong(p0, 0, 29820298)
    assert classify(p0) == "private_from_protected"
    lib.stm_commit_transaction()
    lib.stm_begin_inevitable_transaction()
    assert classify(p0) == "protected"
    assert lib.getlong(p0, 0) == 29820298
    assert classify(p0) == "protected"

def test_old_private_from_protected_to_young_private():
    p0 = oalloc_refs(1)
    assert classify(p0) == "protected"
    p1 = nalloc(HDR)
    lib.setptr(p0, 0, p1)
    assert classify(p0) == "private_from_protected"
    lib.stm_push_root(p0)
    minor_collect()
    p0b = lib.stm_pop_root()
    assert p0b == p0
    check_nursery_free(p1)
    assert classify(p0) == "private_from_protected"
    p2 = lib.getptr(p0, 0)
    assert not lib.in_nursery(p2)
    check_not_free(p2)
    assert classify(p2) == "private"

def test_old_private_from_protected_to_young_private_2():
    py.test.skip("not valid")
    p0 = nalloc_refs(1)
    lib.stm_commit_transaction()
    lib.stm_begin_inevitable_transaction()
    lib.setptr(p0, 0, ffi.NULL)
    assert classify(p0) == "private_from_protected"
    assert lib.in_nursery(p0)      # a young private_from_protected
    #
    lib.stm_push_root(p0)
    minor_collect()
    p0 = lib.stm_pop_root()
    assert classify(p0) == "private_from_protected"
    assert not lib.in_nursery(p0)  # becomes an old private_from_protected
    #
    # Because it's a private_from_protected, its h_revision is a pointer
    # to the backup copy, and not stm_private_rev_num.  It means that the
    # write barrier will always enter its slow path, even though the
    # GCFLAG_WRITE_BARRIER is not set.
    assert p0.h_revision != lib.get_private_rev_num()
    assert not (p0.h_tid & GCFLAG_WRITE_BARRIER)
    #
    p1 = nalloc(HDR)
    lib.setptr(p0, 0, p1)          # should trigger the write barrier again
    assert classify(p0) == "private_from_protected"
    lib.stm_push_root(p0)
    minor_collect()
    p0b = lib.stm_pop_root()
    assert p0b == p0
    check_nursery_free(p1)
    assert classify(p0) == "private_from_protected"
    p2 = lib.getptr(p0, 0)
    assert not lib.in_nursery(p2)
    check_not_free(p2)
    assert classify(p2) == "private"

def test_old_private_from_protected_to_young_private_3():
    p0 = palloc_refs(1)
    pw = lib.stm_write_barrier(p0)
    lib.stm_commit_transaction()
    lib.stm_begin_inevitable_transaction()
    pr = lib.stm_read_barrier(p0)
    assert classify(pr) == "protected"
    assert lib.in_nursery(pr) # a young protected
    #
    minor_collect()
    # each minor collect adds WRITE_BARRIER to protected/private
    # objects it moves out of the nursery
    pr = lib.stm_read_barrier(p0)
    assert pr.h_tid & GCFLAG_WRITE_BARRIER
    pw = lib.stm_write_barrier(pr)
    # added to old_obj_to_trace
    assert not (pw.h_tid & GCFLAG_WRITE_BARRIER)

    lib.setptr(pw, 0, ffi.NULL)
    assert classify(pw) == "private_from_protected"
    assert not lib.in_nursery(pw)

    assert pw.h_revision != lib.get_private_rev_num()
    assert not (pw.h_tid & GCFLAG_WRITE_BARRIER)
    # #
    
    lib.stm_push_root(pw)
    minor_collect()
    p1 = nalloc(HDR)
    pw = lib.stm_pop_root()
    assert pw.h_tid & GCFLAG_WRITE_BARRIER
    lib.setptr(pw, 0, p1)          # should trigger the write barrier again
    assert classify(pr) == "private_from_protected"
    minor_collect()
    check_nursery_free(p1)
    pr = lib.stm_read_barrier(p0)
    assert classify(pr) == "private_from_protected"
    p2 = lib.getptr(pr, 0)
    assert not lib.in_nursery(p2)
    check_not_free(p2)
    assert classify(p2) == "private"

def test_new_version():
    p1 = oalloc(HDR)
    assert lib.stm_write_barrier(p1) == p1
    lib.stm_push_root(p1)
    transaction_break()
    p1b = lib.stm_pop_root()
    assert p1b == p1
    p2 = lib.stm_write_barrier(p1)
    assert p2 == p1
    assert not lib.in_nursery(p2)
    check_not_free(p1)
    lib.stm_push_root(p1)
    minor_collect()
    p1b = lib.stm_pop_root()
    assert p1b == p1
    check_not_free(p1)
    p2 = lib.stm_read_barrier(p1)
    assert p2 == p1
    assert not lib.in_nursery(p2)
    assert classify(p2) == "private_from_protected"

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
    assert classify(p2) == "private"
    p3 = lib.stm_write_barrier(p1)
    assert classify(p3) == "private"
    assert p3 == p2 != p1
    assert not lib.in_nursery(p2)

def _public_and_protected_in_nursery():
    p1 = palloc(HDR + WORD)
    lib.rawsetlong(p1, 0, 1000000)
    p2 = lib.stm_write_barrier(p1)
    assert lib.in_nursery(p2)
    lib.setlong(p1, 0, 2000000)
    assert lib.rawgetlong(p2, 0) == 2000000
    lib.stm_commit_transaction()
    lib.stm_begin_inevitable_transaction()
    assert classify(p1) == "public"
    assert classify(p2) == "protected"
    check_not_free(p2)
    return p1, p2

def test_public_not_in_nursery():
    p1, p2 = _public_and_protected_in_nursery()
    plist = []
    def f1(_):
        p3 = lib.stm_read_barrier(p1)
        assert classify(p3) == "public"
        assert not lib.in_nursery(p3)
        assert p3 != p2    # not in-place, because p2 is in the nursery
        plist.append(p3)
    run_parallel(f1)
    p3 = lib.stm_read_barrier(p1)
    assert plist == [p3]
    assert classify(p3) == "public"
    assert not lib.in_nursery(p3)

def test_move_to_one_place():
    p1 = nalloc(HDR)
    lib.stm_push_root(p1)
    lib.stm_push_root(p1)
    lib.stm_push_root(p1)
    minor_collect()
    p2a = lib.stm_pop_root()
    p2b = lib.stm_pop_root()
    p2c = lib.stm_pop_root()
    assert p2a == p2b == p2c

def test_backup_ptr_update():
    p1 = nalloc_refs(1)
    p2 = nalloc(HDR + WORD)
    lib.setlong(p2, 0, 389719)
    lib.setptr(p1, 0, p2)
    lib.stm_push_root(p1)
    assert lib.in_nursery(p1)

    @perform_transaction
    def run(retry_counter):
        if retry_counter == 0:
            lib.stm_write_barrier(p1)
            minor_collect()
            abort_and_retry()

    p1 = lib.stm_pop_root()
    assert not lib.in_nursery(p1)
    p2 = lib.getptr(p1, 0)
    assert lib.getlong(p2, 0) == 389719

def test_nalloc_large_object():
    for repeat in range(3):
        for words in range(40, 81):
            p1 = nalloc_refs(words)
            for c in range(words):
                assert lib.rawgetlong(p1, c) == 0     # null-initialized
                lib.rawsetptr(p1, c, p1)
        p2 = nalloc(HDR)   # this alone should not collect
        check_not_free(p1)
        lib.rawsetptr(p1, 79, p2)
        lib.stm_push_root(p1)
        minor_collect()
        p1b = lib.stm_pop_root()
        assert p1b == p1
        check_not_free(p1)
        assert lib.rawgetptr(p1, 0) == p1
        check_not_free(lib.rawgetptr(p1, 79))
        major_collect()

def test_collect_soon():
    lib.stmgc_minor_collect_soon()
    nalloc(HDR)
