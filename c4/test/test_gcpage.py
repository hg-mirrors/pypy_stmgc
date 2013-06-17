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
