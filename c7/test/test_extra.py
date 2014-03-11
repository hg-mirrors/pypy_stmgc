from support import *
import py

def ffi_new_aligned(string):
    ALIGN = ffi.sizeof("void *")
    p1 = ffi.new("void *[]", (len(string) + ALIGN) // ALIGN)
    p2 = ffi.gc(ffi.cast("char *", p1), lambda p2: p1)
    p2[0:len(string)+1] = string + '\x00'
    assert ffi.string(p2) == string
    return p2


class TestExtra(BaseTest):

    def test_clear_on_abort(self):
        p = ffi.new("char[]", "hello")
        tl = self.get_stm_thread_local()
        tl.mem_clear_on_abort = p
        tl.mem_bytes_to_clear_on_abort = 2
        #
        self.start_transaction()
        assert ffi.string(p) == "hello"
        self.abort_transaction()
        assert p[0] == '\0'
        assert p[1] == '\0'
        assert p[2] == 'l'
        assert p[3] == 'l'
        assert p[4] == 'o'

    def test_call_on_abort(self):
        p0 = ffi_new_aligned("aaa")
        p1 = ffi_new_aligned("hello")
        p2 = ffi_new_aligned("removed")
        p3 = ffi_new_aligned("world")
        #
        @ffi.callback("void(void *)")
        def clear_me(p):
            p = ffi.cast("char *", p)
            p[0] = chr(ord(p[0]) + 1)
        #
        self.start_transaction()
        lib.stm_call_on_abort(p0, clear_me)
        # the registered callbacks are removed on
        # successful commit
        self.commit_transaction()
        assert ffi.string(p0) == "aaa"
        #
        self.start_transaction()
        lib.stm_call_on_abort(p1, clear_me)
        lib.stm_call_on_abort(p2, clear_me)
        lib.stm_call_on_abort(p3, clear_me)
        lib.stm_call_on_abort(p2, ffi.NULL)
        assert ffi.string(p0) == "aaa"
        assert ffi.string(p1) == "hello"
        assert ffi.string(p2) == "removed"
        assert ffi.string(p3) == "world"
        self.abort_transaction()
        #
        assert ffi.string(p0) == "aaa"
        assert ffi.string(p1) == "iello"
        assert ffi.string(p2) == "removed"
        assert ffi.string(p3) == "xorld"
        #
        # the registered callbacks are removed on abort
        self.start_transaction()
        self.abort_transaction()
        assert ffi.string(p0) == "aaa"
        assert ffi.string(p1) == "iello"
        assert ffi.string(p2) == "removed"
        assert ffi.string(p3) == "xorld"
