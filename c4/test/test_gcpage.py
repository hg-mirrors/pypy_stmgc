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
