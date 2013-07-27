import py
from support import *


def setup_function(f):
    lib.stm_clear_between_tests()
    lib.stm_initialize_tests(getattr(f, 'max_aborts', 0))

def teardown_function(_):
    lib.stm_finalize()


def test_HDR():
    import struct
    assert HDR == struct.calcsize("PPP")

def test_malloc_simple():
    assert count_pages() == 0
    p1 = lib.stmgcpage_malloc(HDR)
    print p1
    p2 = lib.stmgcpage_malloc(HDR)
    print p2
    p3 = lib.stmgcpage_malloc(HDR)
    print p3
    assert count_pages() == 1
    p4 = lib.stmgcpage_malloc(HDR + 1)
    print p4
    assert count_pages() == 2
    p5 = lib.stmgcpage_malloc(HDR + 1)
    print p5
    assert distance(p1, p2) == HDR
    assert distance(p2, p3) == HDR
    assert abs(distance(p3, p4)) > PAGE_ROOM / 2
    assert distance(p4, p5) == HDR + WORD

def test_malloc_page_full():
    plist = []
    for i in range(PAGE_ROOM // HDR):
        plist.append(lib.stmgcpage_malloc(HDR))
    for p1, p2 in zip(plist[:-1], plist[1:]):
        assert distance(p1, p2) == HDR
    assert count_pages() == 1
    p = lib.stmgcpage_malloc(HDR)
    assert distance(plist[-1], p) != HDR
    assert count_pages() == 2
    assert count_global_pages() == 2

def test_thread_local_malloc():
    assert count_global_pages() == 0
    where = []
    def f1(r):
        where.append(oalloc(HDR))
    def f2(r):
        where.append(oalloc(HDR))
    run_parallel(f1, f2)
    assert count_pages() == 0
    assert count_global_pages() == 2
    p3, p4 = where
    assert abs(distance(p3, p4)) > PAGE_ROOM / 2

def test_malloc_reuse():
    p1 = oalloc(HDR)
    ofree(p1)
    p2 = oalloc(HDR)
    assert p2 == p1

def test_move_away_as_full_pages():
    assert count_global_pages() == 0
    oalloc(HDR)
    assert count_pages() == 1
    lib.stm_finalize()
    assert count_global_pages() == 1
    lib.stm_initialize_and_set_max_abort(0)    # reuse the same
    assert count_global_pages() == 1
    assert count_pages() == 1

def test_move_away_as_full_pages_2():
    def f1(r):
        assert count_global_pages() == 0
        oalloc(HDR)
        assert count_pages() == 1
        return 2
    def f2(r):
        r.wait(2)
        assert count_global_pages() == 1
        assert count_pages() == 0
        oalloc(HDR)
        assert count_global_pages() == 2
        assert count_pages() == 1
    run_parallel(f1, f2)

def test_free_unused_global_pages():
    def f1(r):
        oalloc(HDR)
        return 2
    def f2(r):
        r.wait(2)
        assert count_global_pages() == 1
        major_collect()
        assert count_global_pages() == 0
    run_parallel(f1, f2)

def test_free_unused_local_pages():
    p1 = oalloc(HDR)
    assert count_pages() == 1
    major_collect()
    assert count_pages() == 0

def test_free_all_unused_local_pages():
    def f1(r):
        p1 = oalloc(HDR)
        assert count_pages() == 1
        r.set(1)
        r.wait(2)
        assert count_pages() == 0
    def f2(r):
        p2 = oalloc(HDR)
        r.wait(1)
        assert count_pages() == 1
        major_collect()
        assert count_pages() == 0
        r.set(2)
    run_parallel(f1, f2)

def test_keep_local_roots_alive():
    p1 = oalloc(HDR)
    assert count_pages() == 1
    lib.stm_push_root(p1)
    major_collect()
    p2 = lib.stm_pop_root()
    assert p2 == p1    # oalloc() does not use the nursery
    assert count_pages() == 1
    p3 = oalloc(HDR)
    p4 = oalloc(HDR)
    assert p2 != p3 != p4 != p2

def test_trace_simple():
    p1 = oalloc_refs(1)
    p2 = oalloc(HDR)
    rawsetptr(p1, 0, p2)
    lib.stm_push_root(p1)
    major_collect()
    p1b = lib.stm_pop_root()
    assert p1b == p1    # oalloc() does not use the nursery
    assert count_pages() == 2   # one for p1, one for p2, which have != sizes

def test_keep_global_roots_alive_2():
    p = oalloc_refs(3)
    rawsetptr(p, 0, p)
    rawsetptr(p, 1, ffi.NULL)
    rawsetptr(p, 2, p)
    lib.stm_push_root(p)
    for i in range(3):
        major_collect()
        check_not_free(p)
        assert rawgetptr(p, 0) == p
        assert rawgetptr(p, 1) == ffi.NULL
        assert rawgetptr(p, 2) == p
    lib.stm_pop_root()

def test_local_copy_from_global_obj():
    p1 = oalloc(HDR + WORD); make_public(p1)
    p2n = lib.stm_write_barrier(p1)
    assert p2n != p1
    assert lib.stm_write_barrier(p1) == p2n
    check_not_free(p1)
    check_not_free(p2n)
    assert p1.h_tid & GCFLAG_PUBLIC_TO_PRIVATE
    lib.stm_push_root(p1)
    lib.stm_push_root(p2n)
    minor_collect()    # move p2n out of the nursery
    p2 = lib.stm_pop_root()
    assert p2 != p2n
    assert p1.h_tid & GCFLAG_PUBLIC_TO_PRIVATE
    assert lib.stm_write_barrier(p1) == p2
    print p1, p2
    major_collect()
    major_collect()
    p1a = lib.stm_pop_root()
    assert p1a == p1
    check_not_free(p1)
    check_not_free(p2)
    p3 = lib.stm_write_barrier(p1)
    assert p3 == p2

def test_new_version():
    p1 = oalloc(HDR + WORD); make_public(p1)
    p2 = oalloc(HDR + WORD); make_public(p2)
    delegate(p1, p2)
    check_not_free(p1)
    check_not_free(p2)
    lib.stm_push_root(p1)
    major_collect()
    major_collect()
    p1b = lib.stm_pop_root()
    assert p1b == p1
    check_not_free(p1)
    check_free_old(p2)
    p3 = lib.stm_write_barrier(p1)
    assert p3 != p1
    assert p3 == lib.stm_write_barrier(p1)

def test_new_version_id_alive():
    p1 = oalloc(HDR + WORD); make_public(p1)
    p2 = oalloc(HDR + WORD); make_public(p2)
    delegate(p1, p2)
    lib.stm_push_root(p1)
    major_collect()
    major_collect()
    p1b = lib.stm_pop_root()
    check_not_free(p1) # id copy
    check_free_old(p2)
    assert p1b == p1

    
def test_new_version_kill_intermediate():
    p1 = oalloc(HDR + WORD); make_public(p1)
    p2 = oalloc(HDR + WORD); make_public(p2)
    p3 = oalloc(HDR + WORD); make_public(p3)
    p4 = oalloc(HDR + WORD); make_public(p4)
    delegate(p1, p2)
    delegate(p2, p3)
    delegate(p3, p4)
    lib.stm_push_root(p2)
    major_collect()
    major_collect()
    p2b = lib.stm_pop_root()
    assert p2b == p1
    check_not_free(p1)
    check_free_old(p2)
    check_free_old(p3)
    check_free_old(p4)
    p5 = lib.stm_write_barrier(p1)
    assert p5 != p1
    assert p5 == lib.stm_write_barrier(p1)
    assert p5 == lib.stm_write_barrier(p5)

def test_new_version_kill_intermediate_non_root():
    p1 = oalloc_refs(1); make_public(p1)
    p2 = oalloc(HDR + WORD); make_public(p2)
    p3 = oalloc(HDR + WORD); make_public(p3)
    p4 = oalloc(HDR + WORD); make_public(p4)
    p5 = oalloc(HDR + WORD); make_public(p5)
    delegate(p2, p3)
    delegate(p3, p4)
    delegate(p4, p5)
    rawsetptr(p1, 0, p3)
    assert rawgetptr(p1, 0) == p3
    lib.stm_push_root(p1)
    major_collect()
    lib.stm_pop_root()
    check_not_free(p1)
    check_not_free(p2)
    check_free_old(p3)
    check_free_old(p4)
    check_free_old(p5)
    print 'p1:', p1
    print '      containing:', rawgetptr(p1, 0)
    print 'p2:', p2
    print 'p3:', p3
    print 'p4:', p4
    print 'p5:', p5
    assert rawgetptr(p1, 0) == p2

def test_new_version_not_kill_intermediate_original():
    p1 = oalloc_refs(1); make_public(p1)
    p2 = oalloc(HDR + WORD); make_public(p2)
    p3 = oalloc(HDR + WORD); make_public(p3)
    p4 = oalloc(HDR + WORD); make_public(p4)
    p5 = oalloc(HDR + WORD); make_public(p5)
    delegate(p2, p3)
    delegate(p3, p4)
    delegate(p4, p5)
    rawsetptr(p1, 0, p3)
    lib.rawsetlong(p2, 0, 222)
    lib.rawsetlong(p3, 0, 333)
    lib.rawsetlong(p4, 0, 444)
    lib.rawsetlong(p5, 0, 555)

    lib.stm_push_root(p1)
    major_collect()
    lib.stm_pop_root()
    check_not_free(p1)
    check_not_free(p2)
    check_free_old(p3)
    check_free_old(p4)
    check_free_old(p5)
    assert rawgetptr(p1, 0) == p2
    assert lib.rawgetlong(p2, 0) == 555   # copied over from p5
    
def test_prebuilt_version_1():
    p1 = lib.pseudoprebuilt(HDR, 42 + HDR)
    check_prebuilt(p1)
    major_collect()
    check_prebuilt(p1)

def test_prebuilt_version_2():
    p1 = lib.pseudoprebuilt(HDR + WORD, 42 + HDR + WORD)
    p2 = oalloc(HDR + WORD); make_public(p2)
    p3 = oalloc(HDR + WORD); make_public(p3)
    delegate(p1, p2)
    delegate(p2, p3)
    major_collect()
    check_prebuilt(p1)
    check_free_old(p2)
    check_free_old(p3)

def test_prebuilt_with_hash():
    p1 = lib.pseudoprebuilt_with_hash(HDR + WORD, 42 + HDR + WORD, 99)
    p2 = oalloc(HDR + WORD); make_public(p2)
    p3 = oalloc(HDR + WORD); make_public(p3)
    delegate(p1, p2)
    delegate(p2, p3)
    major_collect()
    check_prebuilt(p1)
    assert lib.stm_hash(p1) == 99
    check_free_old(p2)
    check_free_old(p3)

def test_prebuilt_version_to_protected():
    p1 = lib.pseudoprebuilt(HDR + WORD, 42 + HDR + WORD)
    p2 = lib.stm_write_barrier(p1)
    lib.stm_commit_transaction()
    lib.stm_begin_inevitable_transaction()
    minor_collect()
    p2 = lib.stm_read_barrier(p1)
    assert p2 != p1
    minor_collect()
    major_collect()
    check_prebuilt(p1)
    check_not_free(p2)     # XXX replace with p1

def test_prebuilt_version_to_protected_copy_over_prebuilt():
    py.test.skip("""current copy-over-prebuilt-original approach
    does not work with public_prebuilt->stub->protected""")
    p1 = lib.pseudoprebuilt(HDR, 42 + HDR)
    p2 = lib.stm_write_barrier(p1)
    lib.stm_commit_transaction()
    lib.stm_begin_inevitable_transaction()
    minor_collect()
    p2 = lib.stm_read_barrier(p1)
    assert p2 != p1
    minor_collect()
    major_collect()
    major_collect()
    print classify(p2)
    check_prebuilt(p1)
    check_free_old(p2)


def test_private():
    p1 = nalloc(HDR)
    lib.stm_push_root(p1)
    minor_collect()
    major_collect()
    p1 = lib.stm_pop_root()
    assert not lib.in_nursery(p1)
    check_not_free(p1)

def test_major_collect_first_does_minor_collect():
    p1 = nalloc(HDR)
    lib.stm_push_root(p1)
    major_collect()
    p1 = lib.stm_pop_root()
    assert not lib.in_nursery(p1)
    check_not_free(p1)

def test_private_from_protected_young():
    p1 = nalloc(HDR + WORD)
    lib.stm_commit_transaction()
    lib.stm_begin_inevitable_transaction()
    p1b = lib.stm_write_barrier(p1)
    assert p1b == p1
    check_not_free(follow_revision(p1))
    assert follow_revision(p1).h_tid & GCFLAG_BACKUP_COPY
    lib.stm_push_root(p1)
    major_collect()
    p1 = lib.stm_pop_root()
    assert not lib.in_nursery(p1)
    check_not_free(p1)
    p1b = lib.stm_write_barrier(p1)
    assert p1b == p1
    check_not_free(follow_revision(p1))
    assert follow_revision(p1).h_tid & GCFLAG_BACKUP_COPY

def test_backup_stolen():
    p = palloc(HDR + WORD)
    def f1(r):
        p1 = lib.stm_write_barrier(p)   # private copy
        lib.stm_push_root(p1)
        minor_collect()
        p1 = lib.stm_pop_root()
        lib.stm_commit_transaction()
        lib.stm_begin_inevitable_transaction()
        assert classify(p) == "public"
        assert classify(p1) == "protected"
        assert classify(follow_revision(p)) == "stub"
        assert p1.h_revision & 1
        def cb(c):
            assert c == 0
            p1b = lib.stm_write_barrier(p1)
            assert p1b == p1
            assert not lib.in_nursery(p1)
            assert classify(p1) == "private_from_protected"
            assert classify(follow_revision(p1)) == "backup"
            lib.stm_push_root(p1)
            r.wait_while_in_parallel()
            p1b = lib.stm_pop_root()
            assert p1b == p1
            check_not_free(p1)
            assert classify(p1) == "private_from_protected"
            assert classify(follow_revision(p1)) == "public"  # has been stolen
            # leave time for f2 to finish, before we commit changes to p1
            # (which has got in its read set)
            r.wait_while_in_parallel()
        perform_transaction(cb)
    def f2(r):
        def cb(c):
            assert c == 0
            r.enter_in_parallel()
            p2 = lib.stm_read_barrier(p)    # steals
            assert classify(p2) == "public"
            print p2
            major_collect()
            r.leave_in_parallel()
            r.enter_in_parallel()
        perform_transaction(cb)
        r.leave_in_parallel()
    run_parallel(f1, f2)

def test_private_from_protected_inevitable():
    p1 = nalloc(HDR + WORD)
    lib.stm_commit_transaction()
    lib.stm_begin_inevitable_transaction()
    p1b = lib.stm_write_barrier(p1)
    assert p1b == p1
    major_collect()

def test_private_from_protected_trace_backup():
    p1 = nalloc_refs(1)
    p2 = nalloc(HDR)
    lib.setptr(p1, 0, p2)
    def cb(c):
        if c == 0:
            lib.setptr(p1, 0, ffi.NULL)
            major_collect()
            abort_and_retry()
    lib.stm_push_root(p1)
    perform_transaction(cb)
    p1 = lib.stm_pop_root()
    check_not_free(p1)
    check_not_free(lib.getptr(p1, 0))

def test_prebuilt_modified_during_transaction():
    p1 = palloc(HDR + WORD)
    p2 = nalloc_refs(1)
    lib.setptr(p2, 0, p1)
    lib.stm_push_root(p2)
    major_collect()
    major_collect()
    p1b = lib.stm_write_barrier(p1)
    assert p1b != p1
    major_collect()
    lib.stm_pop_root()
    p1b = lib.stm_read_barrier(p1)
    check_not_free(p1b)

def test_prebuilt_modified_later():
    p1 = palloc(HDR + WORD)
    p2 = nalloc_refs(1)
    lib.setptr(p2, 0, p1)
    lib.stm_push_root(p2)
    major_collect()
    major_collect()
    p1b = lib.stm_write_barrier(p1)
    assert p1b != p1
    lib.stm_commit_transaction()
    lib.stm_begin_inevitable_transaction()
    major_collect()
    p2b = lib.stm_pop_root()
    check_not_free(p2b)
    check_not_free(p1)
    p1b = lib.stm_read_barrier(p1)
    check_not_free(p1b)
    assert p1 != p1b and p1b != p2 and p2 != p1

def test_big_old_object():
    for words in range(80):
        p1 = oalloc(HDR + words * WORD)
    # assert did not crash

def test_big_old_object_free():
    for words in range(1, 80):
        p1 = oalloc(HDR + words * WORD)
        p1b = lib.stm_write_barrier(p1)
        assert p1b == p1
        lib.stm_commit_transaction()
        lib.stm_begin_inevitable_transaction()

def test_big_old_object_collect():
    for words in range(80):
        p1 = oalloc(HDR + words * WORD)
        lib.stm_push_root(p1)
        major_collect()
        p1b = lib.stm_pop_root()
        assert p1b == p1
        check_not_free(p1)
        #
        major_collect()
        check_free_old(p1)
        #
        major_collect()
        check_free_old(p1)

def test_keep_original_alive():
    p2 = oalloc(HDR + WORD); make_public(p2)
    p2b = lib.stm_write_barrier(p2)
    lib.stm_push_root(p2)
    minor_collect()
    p2 = lib.stm_pop_root()
    p2b = lib.stm_write_barrier(p2)
    assert not lib.in_nursery(p2)
    assert not lib.in_nursery(p2b)
    lib.stm_commit_transaction()
    lib.stm_begin_inevitable_transaction()
    assert classify(p2) == "public"
    assert classify(p2b) == "protected"
    assert ffi.cast("gcptr", p2b.h_original) == p2
    lib.stm_push_root(p2b)
    major_collect()
    p2c = lib.stm_pop_root()
    check_not_free(ffi.cast("gcptr", p2c.h_original))
    assert p2c == p2b
    assert ffi.cast("gcptr", p2c.h_original) == p2

def test_more_h_original_1():
    p2 = oalloc_refs(1); make_public(p2)
    lib.stm_push_root(p2)
    def f1(_):
        p1 = oalloc(HDR + WORD)
        lib.rawsetlong(p1, 0, -8922908)
        setptr(p2, 0, p1)
    run_parallel(f1)
    p2 = lib.stm_pop_root()
    p1 = getptr(p2, 0)
    assert classify(p1) == "stub"
    assert not lib.in_nursery(p1)
    p1org = p1.h_original
    assert p1org != 0
    assert classify(ffi.cast("gcptr", p1org)) == "protected"
    assert not lib.in_nursery(ffi.cast("gcptr", p1org))
    assert lib.getlong(p1, 0) == -8922908
    p1r = lib.stm_read_barrier(p1)    # protected->public
    assert classify(p1r) == "public"
    assert p1r == ffi.cast("gcptr", p1org)
    assert p1r.h_original == 0
    #
    lib.stm_push_root(p2)
    major_collect()
    p2 = lib.stm_pop_root()
    check_not_free(ffi.cast("gcptr", p1org))
    p1 = getptr(p2, 0)
    assert p1 == p1r
    assert p1.h_original == 0
    assert classify(p1) == "public"
    assert ffi.cast("gcptr", p1org) == p1
    assert lib.getlong(p1, 0) == -8922908

def test_more_h_original_2():
    p2 = oalloc_refs(1); make_public(p2)
    lib.stm_push_root(p2)
    def f1(_):
        p1 = oalloc(HDR + WORD)
        lib.rawsetlong(p1, 0, -8922908)
        setptr(p2, 0, p1)
    run_parallel(f1)
    p2 = lib.stm_pop_root()
    p1 = getptr(p2, 0)
    assert classify(p1) == "stub"
    assert not lib.in_nursery(p1)
    p1org = p1.h_original
    assert p1org != 0
    assert classify(ffi.cast("gcptr", p1org)) == "protected"
    assert not lib.in_nursery(ffi.cast("gcptr", p1org))
    #
    lib.stm_push_root(p2)
    major_collect()
    p2 = lib.stm_pop_root()
    check_not_free(p1)
    check_not_free(ffi.cast("gcptr", p1org))
    p1b = getptr(p2, 0)
    assert p1b == p1
    assert p1b.h_original == p1org
    assert classify(p1b) == "stub"
    assert lib.getlong(p1, 0) == -8922908
