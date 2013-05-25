import py
from support import *


def setup_function(f):
    lib.stm_clear_between_tests()
    lib.stm_initialize_tests(getattr(f, 'max_aborts', 0))

def teardown_function(_):
    lib.stm_finalize()


def test_HDR():
    import struct
    assert HDR == struct.calcsize("PP")

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

def test_thread_local_malloc():
    init = count_global_pages()
    where = []
    def f1(r):
        where.append(oalloc(HDR))
    def f2(r):
        where.append(oalloc(HDR))
    run_parallel(f1, f2)
    assert count_pages() == 0
    assert count_global_pages() == init + 2
    p3, p4 = where
    assert abs(distance(p3, p4)) > PAGE_ROOM / 2

def test_malloc_reuse():
    p1 = oalloc(HDR)
    ofree(p1)
    p2 = oalloc(HDR)
    assert p2 == p1

def test_move_away_as_full_pages():
    init = count_global_pages()
    oalloc(HDR)
    assert count_pages() == 1
    lib.stm_finalize()
    lib.stm_initialize_tests(0)
    assert count_global_pages() == init + 1
    assert count_pages() == 0

def test_free_unused_global_pages():
    oalloc(HDR)
    lib.stm_finalize()
    lib.stm_initialize_tests(0)
    assert count_global_pages() >= 1
    major_collect()
    assert count_global_pages() == 0
    major_collect()
    assert count_global_pages() == 0

def test_free_unused_local_pages():
    p1 = oalloc(HDR)
    assert count_pages() == 1
    major_collect()
    assert count_pages() == 0
    #p2 = oalloc(HDR)
    #assert p2 == p1

def test_free_all_unused_local_pages():
    def f1(r):
        p1 = oalloc(HDR)
        assert count_pages() == 1
        r.set(1)
        r.wait(2)
        assert count_pages() == 0
        #p4 = oalloc(HDR)
        #assert p4 == p1
    def f2(r):
        p2 = oalloc(HDR)
        r.wait(1)
        assert count_pages() == 1
        major_collect()
        assert count_pages() == 0
        r.set(2)
        #p3 = oalloc(HDR)
        #assert p3 == p2
    run_parallel(f1, f2)

def test_keep_local_roots_alive():
    p1 = oalloc(HDR)
    assert count_pages() == 1
    lib.stm_push_root(p1)
    major_collect()
    p2 = lib.stm_pop_root()
    assert p2 == p1    # no nursery so far
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
    assert p1b == p1    # no nursery so far
    assert count_pages() == 2   # one for p1, one for p2, which have != sizes

def test_keep_global_roots_alive():
    init = count_global_pages()
    p1 = oalloc(HDR)
    lib.stm_finalize()
    lib.stm_initialize_tests(0)
    assert count_pages() == 0
    assert count_global_pages() == init + 1
    lib.stm_push_root(p1)
    major_collect()
    p2 = lib.stm_pop_root()
    assert p2 == p1
    assert count_pages() == 1
    assert count_global_pages() == 0

def test_keep_global_roots_alive_2():
    p = oalloc_refs(3); make_global(p)
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
    p1 = oalloc(HDR)
    assert lib.stm_write_barrier(p1) == p1
    make_global(p1)
    p2n = lib.stm_write_barrier(p1)
    assert p2n != p1
    check_not_free(p1)
    check_not_free(p2n)
    assert p1.h_tid & GCFLAG_PUBLIC_TO_PRIVATE
    lib.stm_push_root(p1)
    lib.stm_push_root(p2n)
    minor_collect()    # move p2n out of the nursery
    p2 = lib.stm_pop_root()
    assert p2 != p2n
    print p1, p2
    major_collect()
    major_collect()
    lib.stm_pop_root()
    check_not_free(p1)
    check_not_free(p2)
    p3 = lib.stm_write_barrier(p1)
    assert p3 == p2

def test_global_obj_from_local_copy():
    p1 = oalloc(HDR)
    assert lib.stm_write_barrier(p1) == p1
    make_global(p1)
    lib.stm_push_root(p1)
    lib.stm_push_root(lib.stm_write_barrier(p1))
    minor_collect()     # move p2n out of the nursery
    p2 = lib.stm_pop_root()
    lib.stm_pop_root()
    assert p2 != p1
    check_not_free(p1)
    check_not_free(p2)
    lib.stm_push_root(p2)
    major_collect()
    major_collect()
    lib.stm_pop_root()
    check_not_free(p1)
    check_not_free(p2)
    p3 = lib.stm_write_barrier(p1)
    assert p3 == p2

def test_new_version():
    p1 = oalloc(HDR)
    make_global(p1)
    p2 = oalloc(HDR)
    make_global(p2)
    delegate(p1, p2)
    check_not_free(p1)
    check_not_free(p2)
    lib.stm_push_root(p1)
    major_collect()
    major_collect()
    p1b = lib.stm_pop_root()
    assert p1b == p2
    check_free(p1)
    check_not_free(p2)
    p3 = lib.stm_write_barrier(p2)
    assert p3 != p2
    assert p3 == lib.stm_write_barrier(p2)

def test_new_version_kill_intermediate():
    p1 = oalloc(HDR); make_global(p1)
    p2 = oalloc(HDR); make_global(p2)
    p3 = oalloc(HDR); make_global(p3)
    p4 = oalloc(HDR); make_global(p4)
    delegate(p1, p2)
    delegate(p2, p3)
    delegate(p3, p4)
    lib.stm_push_root(p2)
    major_collect()
    major_collect()
    p2b = lib.stm_pop_root()
    assert p2b == p4
    check_free(p1)
    check_free(p2)
    check_free(p3)
    check_not_free(p4)
    p5 = lib.stm_write_barrier(p4)
    assert p5 != p4
    assert p5 == lib.stm_write_barrier(p4)
    assert p5 == lib.stm_write_barrier(p5)

def test_new_version_kill_intermediate_non_root():
    p1 = oalloc_refs(1); make_global(p1)
    p2 = oalloc(HDR);    make_global(p2)
    p3 = oalloc(HDR);    make_global(p3)
    p4 = oalloc(HDR);    make_global(p4)
    p5 = oalloc(HDR);    make_global(p5)
    delegate(p2, p3)
    delegate(p3, p4)
    delegate(p4, p5)
    rawsetptr(p1, 0, p3)
    assert rawgetptr(p1, 0) == p3
    lib.stm_push_root(p1)
    major_collect()
    lib.stm_pop_root()
    check_not_free(p1)
    check_free(p2)
    check_free(p3)
    check_free(p4)
    check_not_free(p5)
    print 'p1:', p1
    print '      containing:', rawgetptr(p1, 0)
    print 'p2:', p2
    print 'p3:', p3
    print 'p4:', p4
    print 'p5:', p5
    assert rawgetptr(p1, 0) == p5

def test_prebuilt_version():
    p1 = lib.pseudoprebuilt(HDR, 42 + HDR)
    lib.stm_push_root(lib.stm_write_barrier(p1))
    minor_collect()    # move it out of the nursery
    p2 = lib.stm_pop_root()
    assert p2 != p1
    check_prebuilt(p1)
    check_not_free(p2)
    major_collect()
    major_collect()
    check_prebuilt(p1)
    check_not_free(p2)
    p3 = lib.stm_write_barrier(p1)
    assert p3 == p2

def test_prebuilt_version_global():
    p1 = lib.pseudoprebuilt(HDR, 42 + HDR)
    p2 = oalloc(HDR); make_global(p2)
    p3 = oalloc(HDR); make_global(p3)
    delegate(p1, p2)
    delegate(p2, p3)
    assert lib.stm_read_barrier(p1) == p3
    major_collect()
    major_collect()
    check_prebuilt(p1)
    check_free(p2)
    check_not_free(p3)
    assert lib.stm_read_barrier(p1) == p3

def test_start_many_threads():
    def f1(r):
        major_collect()
    run_parallel(f1, f1, f1, f1, f1, f1, f1, f1)

def test_list_of_read_objects():
    def f1(r):
        def cb(_):
            p1 = oalloc(HDR); make_global(p1)
            p2 = oalloc(HDR); make_global(p2)
            delegate(p1, p2)
            assert lib.stm_read_barrier(p1) == p2
            major_collect()   # drops both p1 and p2, but p2 is in read_objects
            check_free(p1)
            check_free(p2)
        perform_transaction(cb)
    run_parallel(f1, f1, f1)
