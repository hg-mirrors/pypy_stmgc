import py, sys, struct
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
            assert ffi.string(c).endswith("eli-421289712eee")

def test_inspect_abort_info_nested_unsigned():
    fo1 = ffi.new("long[]", [-2, 2, HDR, 0])
    fo2 = ffi.new("long[]", [2, HDR + WORD, -1, 0])
    #
    @perform_transaction
    def run(retry_counter):
        if retry_counter == 0:
            p = nalloc(HDR + WORD)
            q = nalloc(HDR + 2 * WORD)
            lib.setlong(p, 0, sys.maxint)
            lib.setlong(q, 1, -1)
            lib.stm_abort_info_push(p, fo1)
            lib.stm_abort_info_push(q, fo2)
            abort_and_retry()
        else:
            c = lib.stm_inspect_abort_info()
            assert c
            assert ffi.string(c).endswith("eli%dei%deee" % (
                sys.maxint, sys.maxint * 2 + 1))

def test_inspect_abort_info_string():
    fo1 = ffi.new("long[]", [3, HDR, HDR + 1, 0])
    #
    @perform_transaction
    def run(retry_counter):
        if retry_counter == 0:
            p = nalloc_refs(1)
            q = nalloc(HDR + 2 * WORD)
            lib.setptr(p, 0, q)
            lib.setlong(q, 0, -937251491)
            lib.setlong(q, 1, -389541051)
            lib.stm_abort_info_push(p, fo1)
            abort_and_retry()
        else:
            c = lib.stm_inspect_abort_info()
            assert c
            expected = struct.pack("ll", -937251491, -389541051)
            assert ffi.string(c).endswith("e%d:%se" % (
                len(expected) - 1, expected[1:]))

def test_inspect_null():
    fo1 = ffi.new("long[]", [3, HDR, HDR + 1, 0])
    #
    @perform_transaction
    def run(retry_counter):
        if retry_counter == 0:
            p = nalloc_refs(1)
            lib.setptr(p, 0, ffi.NULL)    # default
            lib.stm_abort_info_push(p, fo1)
            abort_and_retry()
        else:
            c = lib.stm_inspect_abort_info()
            assert c
            assert ffi.string(c).endswith("e0:e")

def test_latest_version():
    fo1 = ffi.new("long[]", [1, HDR, 0])
    p = palloc(HDR + WORD)
    lib.rawsetlong(p, 0, -9827892)
    #
    @perform_transaction
    def run(retry_counter):
        if retry_counter == 0:
            lib.stm_abort_info_push(p, fo1)
            lib.setlong(p, 0, 424242)
            abort_and_retry()
        else:
            c = lib.stm_inspect_abort_info()
            assert c
            assert ffi.string(c).endswith("ei424242ee")
