import py
from support import *


def setup_function(f):
    lib.stm_clear_between_tests()
    lib.stm_initialize_tests(getattr(f, 'max_aborts', 0))

def teardown_function(_):
    lib.stm_finalize()


def test_abort_info_stack():
    p = nalloc(HDR)
    q = nalloc(HDR)
    lib.stm_abort_info_push(p, ffi.cast("void *", 123))
    lib.stm_abort_info_push(q, ffi.cast("void *", 125))
    lib.stm_abort_info_pop(2)
    # no real test here
