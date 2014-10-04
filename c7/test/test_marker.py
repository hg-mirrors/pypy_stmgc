from support import *
import py, time


class TestMarker(BaseTest):

    def recording(self, kind):
        seen = []
        @ffi.callback("stmcb_timing_event_fn")
        def timing_event(tl, event, markers):
            if event == kind:
                seen.append(tl)
                seen.append(markers[0].tl)
                seen.append(markers[0].segment_base)
                seen.append(markers[0].odd_number)
                seen.append(markers[0].object)
                seen.append(markers[1].tl)
                seen.append(markers[1].segment_base)
                seen.append(markers[1].odd_number)
                seen.append(markers[1].object)
        lib.stmcb_timing_event = timing_event
        self.timing_event_keepalive = timing_event
        self.seen = seen

    def check_recording(self, i1, o1, i2, o2):
        seen = self.seen
        assert seen[0] == self.tls[1]
        segbase = lib._stm_get_segment_base
        assert seen[1:5] == [self.tls[1], segbase(2), i1, o1]
        assert seen[5:9] == [self.tls[0], segbase(1), i2, o2]
        assert len(seen) == 9

    def test_marker_odd_simple(self):
        self.start_transaction()
        self.push_root(ffi.cast("object_t *", 29))
        stm_minor_collect()
        stm_major_collect()
        # assert did not crash
        x = self.pop_root()
        assert int(ffi.cast("uintptr_t", x)) == 29

    def test_abort_marker_no_shadowstack(self):
        self.recording(lib.STM_CONTENTION_WRITE_WRITE)
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
        self.check_recording(21, ffi.NULL, 19, ffi.NULL)

    def test_double_remote_markers_cb_write_read(self):
        self.recording(lib.STM_CONTENTION_WRITE_READ)
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
        self.check_recording(21, ffi.NULL, 0, ffi.NULL)
