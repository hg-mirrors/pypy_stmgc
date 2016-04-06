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
        self.commit_transaction()
        assert ffi.string(p) == "hello"
        #
        self.start_transaction()
        assert ffi.string(p) == "hello"
        self.abort_transaction()
        assert p[0] == '\0'
        assert p[1] == '\0'
        assert p[2] == 'l'
        assert p[3] == 'l'
        assert p[4] == 'o'

    def test_reset_on_abort(self):
        p = ffi.new("char[]", "hello")
        tl = self.get_stm_thread_local()
        assert tl.mem_reset_on_abort == ffi.NULL
        tl.mem_reset_on_abort = p
        tl.mem_bytes_to_reset_on_abort = 2
        tl.mem_stored_for_reset_on_abort = ffi.new("char[5]")
        #
        self.start_transaction()
        assert ffi.string(p) == "hello"
        p[0] = 'w'
        self.commit_transaction()
        assert ffi.string(p) == "wello"
        #
        self.start_transaction()
        assert ffi.string(p) == "wello"
        p[1] = 'a'
        p[4] = 'i'
        self.abort_transaction()
        assert ffi.string(p) == "welli"

    def test_call_on_abort(self):
        p0 = ffi_new_aligned("aaa")
        p1 = ffi_new_aligned("hello")
        p2 = ffi_new_aligned("removed")
        p3 = ffi_new_aligned("world")
        p4 = ffi_new_aligned("00")
        #
        @ffi.callback("void(void *)")
        def clear_me(p):
            p = ffi.cast("char *", p)
            p[0] = chr(ord(p[0]) + 1)
        #
        self.start_transaction()
        x = lib.stm_call_on_abort(self.get_stm_thread_local(), p0, clear_me)
        assert x != 0
        # the registered callbacks are removed on
        # successful commit
        self.commit_transaction()
        assert ffi.string(p0) == "aaa"
        #
        self.start_transaction()
        x = lib.stm_call_on_abort(self.get_stm_thread_local(), p1, clear_me)
        assert x != 0
        x = lib.stm_call_on_abort(self.get_stm_thread_local(), p2, clear_me)
        assert x != 0
        x = lib.stm_call_on_abort(self.get_stm_thread_local(), p3, clear_me)
        assert x != 0
        x = lib.stm_call_on_abort(self.get_stm_thread_local(), p2, ffi.NULL)
        assert x != 0
        x = lib.stm_call_on_abort(self.get_stm_thread_local(), p2, ffi.NULL)
        assert x == 0
        x = lib.stm_call_on_abort(self.get_stm_thread_local(), p4, ffi.NULL)
        assert x == 0
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
        assert ffi.string(p4) == "00"

    def test_ignores_if_outside_transaction(self):
        @ffi.callback("void(void *)")
        def dont_see_me(p):
            seen.append(p)
        #
        seen = []
        p0 = ffi_new_aligned("aaa")
        x = lib.stm_call_on_abort(self.get_stm_thread_local(), p0, dont_see_me)
        assert x != 0
        self.start_transaction()
        self.abort_transaction()
        assert seen == []

    def test_call_on_commit(self):
        p0 = ffi_new_aligned("aaa")
        p1 = ffi_new_aligned("hello")
        p2 = ffi_new_aligned("removed")
        p3 = ffi_new_aligned("world")
        p4 = ffi_new_aligned("00")
        #
        @ffi.callback("void(void *)")
        def clear_me(p):
            p = ffi.cast("char *", p)
            p[0] = chr(ord(p[0]) + 1)
        #
        self.start_transaction()
        x = lib.stm_call_on_commit(self.get_stm_thread_local(), p0, clear_me)
        assert x != 0
        # the registered callbacks are not called on abort
        self.abort_transaction()
        assert ffi.string(p0) == "aaa"
        #
        self.start_transaction()
        x = lib.stm_call_on_commit(self.get_stm_thread_local(), p1, clear_me)
        assert x != 0
        x = lib.stm_call_on_commit(self.get_stm_thread_local(), p2, clear_me)
        assert x != 0
        x = lib.stm_call_on_commit(self.get_stm_thread_local(), p3, clear_me)
        assert x != 0
        x = lib.stm_call_on_commit(self.get_stm_thread_local(), p2, ffi.NULL)
        assert x != 0
        x = lib.stm_call_on_commit(self.get_stm_thread_local(), p2, ffi.NULL)
        assert x == 0
        x = lib.stm_call_on_commit(self.get_stm_thread_local(), p4, ffi.NULL)
        assert x == 0
        assert ffi.string(p0) == "aaa"
        assert ffi.string(p1) == "hello"
        assert ffi.string(p2) == "removed"
        assert ffi.string(p3) == "world"
        self.commit_transaction()
        #
        assert ffi.string(p0) == "aaa"
        assert ffi.string(p1) == "iello"
        assert ffi.string(p2) == "removed"
        assert ffi.string(p3) == "xorld"
        assert ffi.string(p4) == "00"

    def test_call_on_commit_immediately_if_inevitable(self):
        p0 = ffi_new_aligned("aaa")
        self.start_transaction()
        self.become_inevitable()
        #
        @ffi.callback("void(void *)")
        def clear_me(p):
            p = ffi.cast("char *", p)
            p[0] = chr(ord(p[0]) + 1)
        #
        lib.stm_call_on_commit(self.get_stm_thread_local(), p0, clear_me)
        assert ffi.string(p0) == "baa"
        self.commit_transaction()
        assert ffi.string(p0) == "baa"

    def test_call_on_commit_as_soon_as_inevitable(self):
        p0 = ffi_new_aligned("aaa")
        self.start_transaction()
        #
        @ffi.callback("void(void *)")
        def clear_me(p):
            p = ffi.cast("char *", p)
            p[0] = chr(ord(p[0]) + 1)
        #
        lib.stm_call_on_commit(self.get_stm_thread_local(), p0, clear_me)
        assert ffi.string(p0) == "aaa"
        self.become_inevitable()
        assert ffi.string(p0) == "baa"
        self.commit_transaction()
        assert ffi.string(p0) == "baa"

    def test_call_on_commit_immediately_if_outside_transaction(self):
        p0 = ffi_new_aligned("aaa")
        #
        @ffi.callback("void(void *)")
        def clear_me(p):
            p = ffi.cast("char *", p)
            p[0] = chr(ord(p[0]) + 1)
        #
        lib.stm_call_on_commit(self.get_stm_thread_local(), p0, clear_me)
        assert ffi.string(p0) == "baa"
        self.start_transaction()
        assert ffi.string(p0) == "baa"
        self.commit_transaction()
        assert ffi.string(p0) == "baa"

    def test_stm_become_globally_unique_transaction(self):
        self.start_transaction()
        #
        self.switch(1)
        self.start_transaction()
        self.become_globally_unique_transaction()
        assert self.is_inevitable()
        #
        py.test.raises(Conflict, self.switch, 0)

    def test_stm_stop_all_other_threads_1(self):
        self.start_transaction()
        #
        self.switch(1)
        self.start_transaction()
        self.stop_all_other_threads()
        assert self.is_inevitable()
        #
        py.test.raises(Conflict, self.switch, 0)
        #
        self.switch(1)
        self.resume_all_other_threads()

    def test_stm_stop_all_other_threads_2(self):
        self.start_transaction()
        #
        self.switch(1)
        self.start_transaction()
        self.stop_all_other_threads()
        self.resume_all_other_threads()
        assert self.is_inevitable()
        #
        self.switch(0)   # no conflict
