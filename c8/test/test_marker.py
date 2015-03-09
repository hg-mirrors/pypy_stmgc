from support import *
import py, time


class TestMarker(BaseTest):

    def recording(self, *kinds):
        seen = []
        @ffi.callback("stmcb_timing_event_fn")
        def timing_event(tl, event, marker):
            if len(kinds) > 0 and event not in kinds:
                return
            if marker:
                expanded = (marker.odd_number, marker.object)
            else:
                expanded = None
            seen.append((tl, event, expanded))
        lib.stmcb_timing_event = timing_event
        self.timing_event_keepalive = timing_event
        self.seen = seen

    def check_recording(self, i1, o1, extra=None):
        seen = self.seen
        tl, event, marker = seen[0]
        assert tl == self.tls[1]
        assert marker == (i1, o1)
        if extra is None:
            assert len(seen) == 1
        else:
            assert seen[1] == (self.tls[1], extra, None)
            assert len(seen) == 2

    def test_marker_odd_simple(self):
        self.start_transaction()
        self.push_root(ffi.cast("object_t *", 29))
        stm_minor_collect()
        stm_major_collect()
        # assert did not crash
        x = self.pop_root()
        assert int(ffi.cast("uintptr_t", x)) == 29

    def test_abort_marker_no_shadowstack(self):
        self.recording(lib.STM_CONTENTION_WRITE_READ)
        p = stm_allocate_old(16)
        #
        self.start_transaction()
        stm_set_char(p, 'A')
        #
        self.switch(1)
        self.start_transaction()
        stm_set_char(p, 'B')
        #
        self.switch(0)
        self.commit_transaction()
        #
        py.test.raises(Conflict, self.switch, 1)
        self.check_recording(0, ffi.NULL)

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

    def test_double_abort_markers_cb_write_write(self):
        self.recording(lib.STM_CONTENTION_WRITE_READ)
        p = stm_allocate_old(16)
        p2 = stm_allocate_old(16)
        #
        self.start_transaction()
        self.push_root(ffi.cast("object_t *", 19))
        self.push_root(ffi.cast("object_t *", ffi.NULL))
        stm_set_char(p, 'A')
        self.pop_root()
        self.pop_root()
        self.push_root(ffi.cast("object_t *", 17))
        self.push_root(ffi.cast("object_t *", ffi.NULL))
        stm_set_char(p, 'B')
        stm_set_char(p2, 'C')
        stm_minor_collect()
        #
        self.switch(1)
        self.start_transaction()
        self.push_root(ffi.cast("object_t *", 21))
        self.push_root(ffi.cast("object_t *", ffi.NULL))
        stm_set_char(p, 'B')
        #
        self.switch(0)
        self.commit_transaction()
        #
        py.test.raises(Conflict, self.switch, 1)
        self.check_recording(19, ffi.NULL)

    def test_commit_marker_for_inev(self):
        self.recording(lib.STM_TRANSACTION_COMMIT)
        #
        self.switch(1)
        self.start_transaction()
        self.push_root(ffi.cast("object_t *", 19))
        self.push_root(ffi.cast("object_t *", ffi.NULL))
        self.become_inevitable()
        self.pop_root()
        self.pop_root()
        self.commit_transaction()
        #
        self.check_recording(19, ffi.NULL)

    def test_abort_markers_cb_inevitable(self):
        self.recording(lib.STM_WAIT_OTHER_INEVITABLE)
        #
        self.start_transaction()
        self.become_inevitable()
        #
        self.switch(1)
        self.start_transaction()
        p = stm_allocate(16)
        stm_set_char(p, 'B')
        self.push_root(ffi.cast("object_t *", 21))
        self.push_root(ffi.cast("object_t *", p))
        py.test.raises(Conflict, self.become_inevitable)
        #
        py.test.skip("XXX only during tests does become_inevitable() abort"
                     " and then it doesn't record anything")
        self.check_recording(21, p)

    def test_read_write_contention(self):
        self.recording(lib.STM_CONTENTION_WRITE_READ)
        p = stm_allocate_old(16)
        #
        self.start_transaction()
        self.push_root(ffi.cast("object_t *", 19))
        self.push_root(ffi.cast("object_t *", ffi.NULL))
        stm_set_char(p, 'A')
        self.pop_root()
        self.pop_root()
        self.push_root(ffi.cast("object_t *", 17))
        self.push_root(ffi.cast("object_t *", ffi.NULL))
        #
        self.switch(1)
        self.start_transaction()
        assert stm_get_char(p) == '\x00'
        #
        self.switch(0)
        self.commit_transaction()
        #
        py.test.raises(Conflict, self.switch, 1)
        self.check_recording(19, ffi.NULL)

    def test_all(self):
        self.recording()     # all events
        self.start_transaction()
        self.commit_transaction()
        self.start_transaction()
        stm_major_collect()
        self.abort_transaction()
        assert self.seen == [
            (self.tls[0], lib.STM_TRANSACTION_START,  None),
            (self.tls[0], lib.STM_GC_MINOR_START,     None),
            (self.tls[0], lib.STM_GC_MINOR_DONE,      None),
            (self.tls[0], lib.STM_TRANSACTION_COMMIT, None),
            (self.tls[0], lib.STM_TRANSACTION_START,  None),
            (self.tls[0], lib.STM_GC_MINOR_START,     None),
            (self.tls[0], lib.STM_GC_MINOR_DONE,      None),
            (self.tls[0], lib.STM_GC_MAJOR_START,     None),
            (self.tls[0], lib.STM_GC_MAJOR_DONE,      None),
            (self.tls[0], lib.STM_TRANSACTION_ABORT,  None),
            ]
