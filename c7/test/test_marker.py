from support import *
import py, time

class TestMarker(BaseTest):

    def test_marker_odd_simple(self):
        self.start_transaction()
        self.push_root(ffi.cast("object_t *", 29))
        stm_minor_collect()
        stm_major_collect()
        # assert did not crash
        x = self.pop_root()
        assert int(ffi.cast("uintptr_t", x)) == 29

    def test_abort_marker_no_shadowstack(self):
        tl = self.get_stm_thread_local()
        assert tl.longest_marker_state == lib.STM_TIME_OUTSIDE_TRANSACTION
        assert tl.longest_marker_time == 0.0
        #
        self.start_transaction()
        start = time.time()
        while abs(time.time() - start) <= 0.1:
            pass
        self.abort_transaction()
        #
        tl = self.get_stm_thread_local()
        assert tl.longest_marker_state == lib.STM_TIME_RUN_ABORTED_OTHER
        assert 0.099 <= tl.longest_marker_time <= 0.9
        assert tl.longest_marker_self[0] == '\x00'
        assert tl.longest_marker_other[0] == '\x00'

    def test_abort_marker_shadowstack(self):
        self.start_transaction()
        p = stm_allocate(16)
        self.push_root(ffi.cast("object_t *", 29))
        self.push_root(p)
        start = time.time()
        while abs(time.time() - start) <= 0.1:
            pass
        self.abort_transaction()
        #
        tl = self.get_stm_thread_local()
        assert tl.longest_marker_state == lib.STM_TIME_RUN_ABORTED_OTHER
        assert 0.099 <= tl.longest_marker_time <= 0.9
        assert tl.longest_marker_self[0] == '\x00'
        assert tl.longest_marker_other[0] == '\x00'

    def test_abort_marker_no_shadowstack_cb(self):
        @ffi.callback("void(char *, uintptr_t, object_t *, char *, size_t)")
        def expand_marker(base, number, ptr, outbuf, outbufsize):
            seen.append(1)
        lib.stmcb_expand_marker = expand_marker
        seen = []
        #
        self.start_transaction()
        self.abort_transaction()
        #
        tl = self.get_stm_thread_local()
        assert tl.longest_marker_self[0] == '\x00'
        assert not seen

    def test_abort_marker_shadowstack_cb(self):
        @ffi.callback("void(char *, uintptr_t, object_t *, char *, size_t)")
        def expand_marker(base, number, ptr, outbuf, outbufsize):
            s = '%d %r\x00' % (number, ptr)
            assert len(s) <= outbufsize
            outbuf[0:len(s)] = s
        lib.stmcb_expand_marker = expand_marker
        #
        self.start_transaction()
        p = stm_allocate(16)
        self.push_root(ffi.cast("object_t *", 29))
        self.push_root(p)
        start = time.time()
        while abs(time.time() - start) <= 0.1:
            pass
        self.abort_transaction()
        #
        tl = self.get_stm_thread_local()
        assert tl.longest_marker_state == lib.STM_TIME_RUN_ABORTED_OTHER
        assert 0.099 <= tl.longest_marker_time <= 0.9
        assert ffi.string(tl.longest_marker_self) == '29 %r' % (p,)

    def test_macros(self):
        self.start_transaction()
        p = stm_allocate(16)
        tl = self.get_stm_thread_local()
        lib.stm_push_marker(tl, 29, p)
        p1 = self.pop_root()
        assert p1 == p
        p1 = self.pop_root()
        assert p1 == ffi.cast("object_t *", 29)
        py.test.raises(EmptyStack, self.pop_root)
        #
        lib.stm_push_marker(tl, 29, p)
        lib.stm_update_marker_num(tl, 27)
        p1 = self.pop_root()
        assert p1 == p
        p1 = self.pop_root()
        assert p1 == ffi.cast("object_t *", 27)
        py.test.raises(EmptyStack, self.pop_root)
        #
        lib.stm_push_marker(tl, 29, p)
        self.push_root(p)
        lib.stm_update_marker_num(tl, 27)
        p1 = self.pop_root()
        assert p1 == p
        p1 = self.pop_root()
        assert p1 == p
        p1 = self.pop_root()
        assert p1 == ffi.cast("object_t *", 27)
        py.test.raises(EmptyStack, self.pop_root)
        #
        lib.stm_push_marker(tl, 29, p)
        lib.stm_pop_marker(tl)
        py.test.raises(EmptyStack, self.pop_root)

    def test_stm_expand_marker(self):
        @ffi.callback("void(char *, uintptr_t, object_t *, char *, size_t)")
        def expand_marker(base, number, ptr, outbuf, outbufsize):
            s = '%d %r\x00' % (number, ptr)
            assert len(s) <= outbufsize
            outbuf[0:len(s)] = s
        lib.stmcb_expand_marker = expand_marker
        self.start_transaction()
        p = stm_allocate(16)
        self.push_root(ffi.cast("object_t *", 29))
        self.push_root(p)
        self.push_root(stm_allocate(32))
        self.push_root(stm_allocate(16))
        raw = lib._stm_expand_marker()
        assert ffi.string(raw) == '29 %r' % (p,)

    def test_stmcb_debug_print(self):
        @ffi.callback("void(char *, uintptr_t, object_t *, char *, size_t)")
        def expand_marker(base, number, ptr, outbuf, outbufsize):
            s = '<<<%d>>>\x00' % (number,)
            assert len(s) <= outbufsize
            outbuf[0:len(s)] = s
        @ffi.callback("void(char *, double, char *)")
        def debug_print(cause, time, marker):
            if 0.0 < time < 1.0:
                time = "time_ok"
            seen.append((ffi.string(cause), time, ffi.string(marker)))
        seen = []
        lib.stmcb_expand_marker = expand_marker
        lib.stmcb_debug_print = debug_print
        #
        self.start_transaction()
        p = stm_allocate(16)
        self.push_root(ffi.cast("object_t *", 29))
        self.push_root(p)
        self.abort_transaction()
        #
        assert seen == [("run aborted other", "time_ok", "<<<29>>>")]
