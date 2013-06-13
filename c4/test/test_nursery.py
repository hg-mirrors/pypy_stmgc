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
