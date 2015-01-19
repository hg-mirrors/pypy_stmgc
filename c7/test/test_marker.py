from support import *
import py, time


class TestMarker(BaseTest):

    def recording(self, *kinds):
        seen = []
        @ffi.callback("stmcb_timing_event_fn")
        def timing_event(tl, event, markers):
            if len(kinds) > 0 and event not in kinds:
                return
            if markers:
                expanded = []
                for i in range(2):
                    expanded.append((markers[i].tl,
                                     markers[i].segment_base,
                                     markers[i].odd_number,
                                     markers[i].object))
            else:
                expanded = None
            seen.append((tl, event, expanded))
        lib.stmcb_timing_event = timing_event
        self.timing_event_keepalive = timing_event
        self.seen = seen

    def check_recording(self, i1, o1, i2, o2, extra=None):
        seen = self.seen
        tl, event, markers = seen[0]
        assert tl == self.tls[1]
        segbase = lib._stm_get_segment_base
        assert markers[0] == (self.tls[1], segbase(2), i1, o1)
        assert markers[1] == (self.tls[0], segbase(1), i2, o2)
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
        self.recording(lib.STM_CONTENTION_WRITE_WRITE,
                       lib.STM_WAIT_CONTENTION,
                       lib.STM_ABORTING_OTHER_CONTENTION)
        p = stm_allocate_old(16)
        #
        self.start_transaction()
        stm_set_char(p, 'A')
        #
        self.switch(1)
        self.start_transaction()
        py.test.raises(Conflict, stm_set_char, p, 'B')
        #
        self.check_recording(0, ffi.NULL, 0, ffi.NULL)

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
        self.recording(lib.STM_CONTENTION_WRITE_WRITE)
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
        stm_minor_collect()
        #
        self.switch(1)
        self.start_transaction()
        self.push_root(ffi.cast("object_t *", 21))
        self.push_root(ffi.cast("object_t *", ffi.NULL))
        py.test.raises(Conflict, stm_set_char, p, 'B')
        #
        self.check_recording(21, ffi.NULL, 19, ffi.NULL)

    def test_double_abort_markers_cb_inevitable(self):
        self.recording(lib.STM_CONTENTION_INEVITABLE)
        #
        self.start_transaction()
        p = stm_allocate(16)
        stm_set_char(p, 'A')
        self.push_root(ffi.cast("object_t *", 19))
        self.push_root(ffi.cast("object_t *", p))
        self.become_inevitable()
        self.pop_root()
        self.pop_root()
        self.push_root(ffi.cast("object_t *", 17))
        self.push_root(ffi.cast("object_t *", ffi.NULL))
        stm_minor_collect()
        #
        self.switch(1)
        self.start_transaction()
        p = stm_allocate(16)
        stm_set_char(p, 'B')
        self.push_root(ffi.cast("object_t *", 21))
        self.push_root(ffi.cast("object_t *", p))
        py.test.raises(Conflict, self.become_inevitable)
        #
        self.check_recording(21, p, 19, p)

    def test_read_write_contention(self):
        self.recording(lib.STM_CONTENTION_WRITE_READ)
        p = stm_allocate_old(16)
        #
        self.start_transaction()
        assert stm_get_char(p) == '\x00'
        #
        self.switch(1)
        self.start_transaction()
        self.push_root(ffi.cast("object_t *", 19))
        self.push_root(ffi.cast("object_t *", ffi.NULL))
        stm_set_char(p, 'A')
        self.pop_root()
        self.pop_root()
        self.push_root(ffi.cast("object_t *", 17))
        self.push_root(ffi.cast("object_t *", ffi.NULL))
        py.test.raises(Conflict, self.commit_transaction)
        #
        self.check_recording(19, ffi.NULL, 0, ffi.NULL)

    def test_double_remote_markers_cb_write_write(self):
        self.recording(lib.STM_CONTENTION_WRITE_WRITE,
                       lib.STM_ABORTING_OTHER_CONTENTION)
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
        tl0 = self.get_stm_thread_local()
        #
        self.switch(1)
        self.start_transaction()
        self.become_inevitable()
        self.push_root(ffi.cast("object_t *", 21))
        self.push_root(ffi.cast("object_t *", ffi.NULL))
        stm_set_char(p, 'B')    # aborts in #0
        self.pop_root()
        self.pop_root()
        self.push_root(ffi.cast("object_t *", 23))
        self.push_root(ffi.cast("object_t *", ffi.NULL))
        #
        py.test.raises(Conflict, self.switch, 0)
        #
        self.check_recording(21, ffi.NULL, 19, ffi.NULL,
                             extra=lib.STM_ABORTING_OTHER_CONTENTION)

    def test_double_remote_markers_cb_write_read(self):
        self.recording(lib.STM_CONTENTION_WRITE_READ,
                       lib.STM_ABORTING_OTHER_CONTENTION)
        p = stm_allocate_old(16)
        #
        self.start_transaction()
        assert stm_get_char(p) == '\x00'    # read
        tl0 = self.get_stm_thread_local()
        #
        self.switch(1)
        self.start_transaction()
        self.become_inevitable()
        self.push_root(ffi.cast("object_t *", 21))
        self.push_root(ffi.cast("object_t *", ffi.NULL))
        stm_set_char(p, 'B')                # write, will abort #0
        self.pop_root()
        self.pop_root()
        self.push_root(ffi.cast("object_t *", 23))
        self.push_root(ffi.cast("object_t *", ffi.NULL))
        self.commit_transaction()
        #
        py.test.raises(Conflict, self.switch, 0)
        #
        self.check_recording(21, ffi.NULL, 0, ffi.NULL,
                             extra=lib.STM_ABORTING_OTHER_CONTENTION)

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
