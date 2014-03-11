from support import *
import py

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
