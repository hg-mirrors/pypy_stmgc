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
    lib.stm_abort_info_push(p, ffi.cast("long *", 123))
    lib.stm_abort_info_push(q, ffi.cast("long *", 125))
    lib.stm_abort_info_pop(2)
    # no real test here

def test_inspect_abort_info_signed():
    py.test.skip("in-progress")
    fo1 = ffi.new("long[]", [-2, 1, HDR, -1, 0])
    #
    @perform_transaction
    def run(retry_counter):
        if retry_counter == 0:
            p = nalloc(HDR + WORD)
            lib.setlong(p, 0, -421289712)
            lib.stm_abort_info_push(p, fo1)
            abort_and_retry()
        else:
            c = lib.stm_inspect_abort_info()
            assert c
            assert ffi.string(c) == "???"
