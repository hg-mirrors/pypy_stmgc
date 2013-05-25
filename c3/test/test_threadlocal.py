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

def test_reset():
    p = oalloc(HDR + 1)
    lib.addr_of_thread_local()[0] = p
    lib.stm_finalize()
    lib.stm_initialize_tests(0)
    assert lib.addr_of_thread_local()[0] == ffi.NULL
