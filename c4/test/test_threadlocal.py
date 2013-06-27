from support import *


def setup_function(f):
    lib.stm_clear_between_tests()
    lib.stm_initialize_tests(getattr(f, 'max_aborts', 0))

def teardown_function(_):
    lib.stm_finalize()


def test_threadlocal():
    p = oalloc(HDR + 1)
    lib.addr_of_thread_local()[0] = p
    major_collect()
    check_not_free(p)
    assert lib.addr_of_thread_local()[0] == p

def test_threadlocal_nursery():
    p = nalloc(HDR + WORD)
    lib.rawsetlong(p, 0, 654321)
    lib.addr_of_thread_local()[0] = p
    minor_collect()
    check_nursery_free(p)
    p1 = lib.addr_of_thread_local()[0]
    assert p1 != ffi.NULL
    check_not_free(p1)
    assert lib.rawgetlong(p1, 0) == 654321

def test_reset():
    p = oalloc(HDR + 1)
    lib.addr_of_thread_local()[0] = p
    lib.stm_finalize()
    lib.stm_initialize_tests(0)
    assert lib.addr_of_thread_local()[0] == ffi.NULL

def test_threadlocal_commit():
    p1 = oalloc(HDR)
    lib.addr_of_thread_local()[0] = p1
    #
    @perform_transaction
    def run(retry_counter):
        assert retry_counter == 0
        p2 = nalloc(HDR + 5 * WORD)
        lib.setlong(p2, 4, -2891922)
        lib.addr_of_thread_local()[0] = p2
    #
    p2b = lib.addr_of_thread_local()[0]
    assert p2b != p1
    assert lib.getlong(p2b, 4) == -2891922

def test_threadlocal_abort(case=0):
    p1 = oalloc(HDR + 5 * WORD)
    lib.rawsetlong(p1, 4, 38972389)
    lib.addr_of_thread_local()[0] = p1
    #
    @perform_transaction
    def run(retry_counter):
        if retry_counter == 0:
            p2 = nalloc(HDR)
            lib.addr_of_thread_local()[0] = p2
            if case == 1:
                minor_collect()
            if case == 2:
                major_collect()
            abort_and_retry()
        else:
            check_not_free(p1)
            assert lib.addr_of_thread_local()[0] == p1
    #
    assert lib.addr_of_thread_local()[0] == p1
    assert lib.getlong(p1, 4) == 38972389

def test_threadlocal_abort_minor():
    test_threadlocal_abort(case=1)

def test_threadlocal_abort_major():
    test_threadlocal_abort(case=2)
