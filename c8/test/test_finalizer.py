from support import *
import py


class TestDestructors(BaseTest):

    def setup_method(self, meth):
        BaseTest.setup_method(self, meth)
        #
        @ffi.callback("stm_finalizer_trigger_fn")
        def trigger():
            # triggers not needed for destructors
            assert False
        self._trigger_keepalive = trigger
        triggers = ffi.new("stm_finalizer_trigger_fn*", trigger)
        lib.stm_setup_finalizer_queues(1, triggers)
        triggers = None
        #
        @ffi.callback("void(object_t *)")
        def destructor(obj):
            assert stm_get_obj_size(obj) == 48
            segnum = lib.current_segment_num()
            tlnum = '?'
            for n, tl in enumerate(self.tls):
                if lib._stm_in_transaction(tl):
                    if tl.last_associated_segment_num == segnum:
                        tlnum = n
                        break
            self.destructors_called.append((obj, tlnum))
        self.destructors_called = []
        lib.stmcb_destructor = destructor
        self._destructor_keepalive = destructor

    def teardown_method(self, meth):
        lib.stmcb_destructor = ffi.NULL
        BaseTest.teardown_method(self, meth)

    def expect_finalized(self, objs, from_tlnum=None):
        assert [obj for (obj, tlnum) in self.destructors_called] == objs
        if from_tlnum is not None:
            for obj, tlnum in self.destructors_called:
                assert tlnum == from_tlnum
        self.destructors_called = []

    def test_no_finalizer(self):
        self.start_transaction()
        lp1 = stm_allocate(48)
        self.commit_transaction()
        self.expect_finalized([])

    def test_young_destructor(self):
        self.start_transaction()
        lp1 = stm_allocate(48)
        lib.stm_enable_destructor(lp1)
        self.expect_finalized([])
        self.commit_transaction()
        self.expect_finalized([lp1], from_tlnum=0)

    def test_young_destructor_survives(self):
        self.start_transaction()
        lp1 = stm_allocate(48)
        lib.stm_enable_destructor(lp1)
        self.push_root(lp1)       # stays alive
        self.commit_transaction()
        self.expect_finalized([])

    def test_young_destructor_aborts(self):
        self.start_transaction()
        lp1 = stm_allocate(48)
        lib.stm_enable_destructor(lp1)
        self.expect_finalized([])
        self.abort_transaction()
        self.start_transaction()
        self.expect_finalized([lp1], from_tlnum=0)

    def test_old_destructor(self):
        self.start_transaction()
        lp1 = stm_allocate(48)
        self.push_root(lp1)
        stm_minor_collect()
        lp1 = self.pop_root()
        lib.stm_enable_destructor(lp1)
        self.commit_transaction()
        self.expect_finalized([])

    def test_old_destructor_2(self):
        self.start_transaction()
        lp1 = stm_allocate(48)
        lib.stm_enable_destructor(lp1)
        self.push_root(lp1)
        stm_minor_collect()
        lp1 = self.pop_root()
        self.expect_finalized([])
        stm_major_collect()
        self.expect_finalized([lp1])
        self.commit_transaction()

    def test_old_destructor_survives(self):
        self.start_transaction()
        lp1 = stm_allocate(48)
        lib.stm_enable_destructor(lp1)
        self.push_root(lp1)
        stm_minor_collect()
        lp1 = self.pop_root()
        self.push_root(lp1)
        stm_major_collect()
        self.commit_transaction()
        self.expect_finalized([])

    def test_old_destructor_segment(self):
        self.start_transaction()
        #
        self.switch(1)
        self.start_transaction()
        lp1 = stm_allocate(48)
        lib.stm_enable_destructor(lp1)
        self.push_root(lp1)
        stm_minor_collect()
        lp1 = self.pop_root()
        #
        self.switch(0)
        self.expect_finalized([])
        stm_major_collect()
        self.expect_finalized([lp1], from_tlnum=1)

    def test_old_destructor_aborts(self):
        self.start_transaction()
        lp1 = stm_allocate(48)
        lib.stm_enable_destructor(lp1)
        self.push_root(lp1)
        self.commit_transaction()
        #
        self.start_transaction()
        self.expect_finalized([])
        self.abort_transaction()
        self.expect_finalized([])

    def test_overflow_destructor_aborts(self):
        self.start_transaction()
        lp1 = stm_allocate(48)
        lib.stm_enable_destructor(lp1)
        self.push_root(lp1)
        stm_minor_collect()
        lp1 = self.pop_root()
        self.push_root(lp1)
        self.expect_finalized([])
        self.abort_transaction()
        self.expect_finalized([lp1], from_tlnum=0)


class TestRegularFinalizerQueues(BaseTest):
    expect_content_character = None
    run_major_collect_in_trigger = False

    def setup_method(self, meth):
        BaseTest.setup_method(self, meth)
        #
        @ffi.callback("stm_finalizer_trigger_fn")
        def trigger0():
            self.queues_triggered.append(0)
            if self.run_major_collect_in_trigger:
                stm_major_collect()
        @ffi.callback("stm_finalizer_trigger_fn")
        def trigger1():
            self.queues_triggered.append(1)
            if self.run_major_collect_in_trigger:
                stm_major_collect()
        self._trigger_keepalive = [trigger0, trigger1]
        triggers = ffi.new("stm_finalizer_trigger_fn[]", self._trigger_keepalive)
        lib.stm_setup_finalizer_queues(2, triggers)
        #
        self.queues_triggered = []

    def get_queues_triggered(self):
        res = set(self.queues_triggered)
        self.queues_triggered = []
        return res

    def get_to_finalize(self, qindex):
        obj = lib.stm_next_to_finalize(qindex)
        res = []
        while obj:
            res.append(obj)
            obj = lib.stm_next_to_finalize(qindex)
        return res



    def test_no_finalizer(self):
        self.start_transaction()
        lp1 = stm_allocate(48)
        stm_major_collect()
        assert not self.get_queues_triggered()

    def test_no_finalizer_in_minor_collection(self):
        self.start_transaction()
        lp1 = stm_allocate(48)
        lib.stm_enable_finalizer(0, lp1)
        stm_minor_collect()
        assert not self.get_queues_triggered()

    def test_finalizer_in_major_collection(self):
        self.start_transaction()
        for repeat in range(4):
            lp1 = stm_allocate(48)
            lp2 = stm_allocate(48)
            lp3 = stm_allocate(48)
            if repeat % 2 == 0: # register on young
                lib.stm_enable_finalizer(0, lp1)
                lib.stm_enable_finalizer(1, lp2)
                lib.stm_enable_finalizer(1, lp3)
            assert not self.get_queues_triggered()
            self.push_roots([lp1, lp2, lp3])
            stm_minor_collect()
            lp1, lp2, lp3 = self.pop_roots()
            if repeat % 2 == 1: # register on old
                lib.stm_enable_finalizer(0, lp1)
                lib.stm_enable_finalizer(1, lp2)
                lib.stm_enable_finalizer(1, lp3)
            print repeat, lp1, lp2, lp3
            stm_major_collect()
            assert {0, 1} == self.get_queues_triggered()
            assert [lp1] == self.get_to_finalize(0)
            assert [lp2, lp3] == self.get_to_finalize(1)

    def test_finalizer_from_other_thread(self):
        self.start_transaction()
        lp1 = stm_allocate(48)
        lib.stm_enable_finalizer(0, lp1)
        stm_set_char(lp1, 'H')
        self.expect_content_character = 'H'
        self.push_root(lp1)
        stm_minor_collect()
        lp1 = self.pop_root()
        print lp1
        #
        self.switch(1)
        self.start_transaction()
        stm_major_collect()
        assert not self.get_queues_triggered()
        #
        self.switch(0)
        assert {0} == self.get_queues_triggered()
        assert [lp1] == self.get_to_finalize(0)

    def test_finalizer_ordering(self):
        self.start_transaction()
        lp1 = stm_allocate_refs(1)
        lp2 = stm_allocate_refs(1)
        lp3 = stm_allocate_refs(1)
        lib.stm_enable_finalizer(1, lp1)
        lib.stm_enable_finalizer(1, lp2)
        lib.stm_enable_finalizer(1, lp3)

        print lp1, lp2, lp3
        stm_set_ref(lp3, 0, lp1)
        stm_set_ref(lp1, 0, lp2)

        self.push_roots([lp1, lp2, lp3])
        stm_minor_collect()
        lp1, lp2, lp3 = self.pop_roots()

        stm_major_collect()
        assert {1} == self.get_queues_triggered()
        assert [lp3] == self.get_to_finalize(1)

    def test_finalizer_extra_transaction(self):
        self.start_transaction()
        lp1 = stm_allocate(32)
        lib.stm_enable_finalizer(0, lp1)
        self.push_root(lp1)
        self.commit_transaction()

        self.start_transaction()
        lp1b = self.pop_root()
        # assert lp1b == lp1 <- lp1 can be in nursery now
        assert not self.get_queues_triggered()
        self.commit_transaction() # finalizer-obj moved to global queue
        assert not self.get_queues_triggered()

        self.start_transaction()
        stm_major_collect()
        assert {0} == self.get_queues_triggered()
        self.commit_transaction() # invoke finalizers (triggers again...)
        self.start_transaction()
        assert {0} == self.get_queues_triggered()
        assert [lp1b] == self.get_to_finalize(0)

    def test_run_cb_for_all_threads(self):
        self.start_transaction()
        lp1 = stm_allocate(48)
        lib.stm_enable_finalizer(0, lp1)
        print lp1
        #
        self.switch(1)
        self.start_transaction()
        lp2 = stm_allocate(56)
        lib.stm_enable_finalizer(1, lp2)
        print lp2

        assert not self.get_queues_triggered()
        stm_major_collect()
        assert len(self.get_to_finalize(1)) == 1
        self.switch(0)
        assert self.get_queues_triggered() == {0, 1}
        assert len(self.get_to_finalize(0)) == 1

    def test_run_major_collect_in_trigger(self):
        self.run_major_collect_in_trigger  = True
        self.start_transaction()
        lp1 = stm_allocate(32)
        lp2 = stm_allocate(32)
        lp3 = stm_allocate(32)
        lib.stm_enable_finalizer(0, lp1)
        lib.stm_enable_finalizer(1, lp2)
        lib.stm_enable_finalizer(1, lp3)
        print lp1, lp2, lp3
        stm_major_collect()
        assert 1 == len(self.get_to_finalize(0))
        assert 2 == len(self.get_to_finalize(1))

    def test_new_objects_w_finalizers(self):
        self.switch(2)
        self.start_transaction()
        lp1 = stm_allocate_refs(3)
        lp2 = stm_allocate_refs(3)
        lib.stm_enable_finalizer(0, lp1)
        lib.stm_enable_finalizer(0, lp2)
        stm_set_ref(lp1, 0, lp2)

        self.push_root(lp1)
        stm_minor_collect()
        lp1 = self.pop_root()
        lp2 = stm_get_ref(lp1, 0)
        # lp1, lp2 have WB_EXECUTED objs

        assert not self.get_queues_triggered()
        stm_major_collect()
        assert [lp1] == self.get_to_finalize(0)


class TestOldStyleRegularFinalizer(BaseTest):
    expect_content_character = None
    run_major_collect_in_finalizer = False

    def setup_method(self, meth):
        BaseTest.setup_method(self, meth)
        #
        @ffi.callback("stm_finalizer_trigger_fn")
        def trigger():
            # triggers not needed for oldstyle-finalizer tests
            assert False
        self._trigger_keepalive = trigger
        triggers = ffi.new("stm_finalizer_trigger_fn*", trigger)
        lib.stm_setup_finalizer_queues(1, triggers)
        #
        @ffi.callback("void(object_t *)")
        def finalizer(obj):
            print "finalizing!", obj
            assert stm_get_obj_size(obj) in [16, 32, 48, 56]
            if self.expect_content_character is not None:
                assert stm_get_char(obj) == self.expect_content_character
            self.finalizers_called.append(obj)
            if self.run_major_collect_in_finalizer:
                stm_major_collect()
        self.finalizers_called = []
        lib.stmcb_finalizer = finalizer
        self._finalizer_keepalive = finalizer

    def expect_finalized(self, objs):
        if isinstance(objs, int):
            assert len(self.finalizers_called) == objs
        else:
            print objs
            print self.finalizers_called
            assert self.finalizers_called == objs
        self.finalizers_called = []

    def test_no_finalizer(self):
        self.start_transaction()
        lp1 = stm_allocate(48)
        stm_major_collect()
        self.expect_finalized([])

    def test_no_finalizer_in_minor_collection(self):
        self.start_transaction()
        lp1 = stm_allocate_with_finalizer(48)
        stm_minor_collect()
        self.expect_finalized([])

    def test_finalizer_in_major_collection(self):
        self.start_transaction()
        for repeat in range(2):
            lp1 = stm_allocate_with_finalizer(48)
            lp2 = stm_allocate_with_finalizer(48)
            lp3 = stm_allocate_with_finalizer(48)
            self.expect_finalized([])
            self.push_roots([lp1, lp2, lp3])
            stm_minor_collect()
            lp1, lp2, lp3 = self.pop_roots()
            print repeat, lp1, lp2, lp3
            stm_major_collect()
            self.expect_finalized([lp1, lp2, lp3])

    def test_finalizer_from_other_thread(self):
        self.start_transaction()
        lp1 = stm_allocate_with_finalizer(48)
        stm_set_char(lp1, 'H')
        self.expect_content_character = 'H'
        self.push_root(lp1)
        stm_minor_collect()
        lp1 = self.pop_root()
        print lp1
        #
        self.switch(1)
        self.start_transaction()
        stm_major_collect()
        self.expect_finalized([])      # marked as dead, but wrong thread
        #
        self.switch(0)
        self.expect_finalized([lp1])   # now it has been finalized

    def test_finalizer_ordering(self):
        self.start_transaction()
        lp1 = stm_allocate_with_finalizer_refs(1)
        lp2 = stm_allocate_with_finalizer_refs(1)
        lp3 = stm_allocate_with_finalizer_refs(1)
        print lp1, lp2, lp3
        stm_set_ref(lp3, 0, lp1)
        stm_set_ref(lp1, 0, lp2)

        self.push_roots([lp1, lp2, lp3])
        stm_minor_collect()
        lp1, lp2, lp3 = self.pop_roots()

        stm_major_collect()
        self.expect_finalized([lp3])

    def test_finalizer_extra_transaction(self):
        self.start_transaction()
        lp1 = stm_allocate_with_finalizer(32)
        print lp1
        self.push_root(lp1)
        self.commit_transaction()

        self.start_transaction()
        lp1b = self.pop_root()
        # assert lp1b == lp1 <- lp1 can be in nursery now
        self.expect_finalized([])
        self.commit_transaction() # finalizer-obj moved to global queue
        self.expect_finalized([])

        self.start_transaction()
        stm_major_collect()
        self.expect_finalized([])
        self.commit_transaction() # invoke finalizers
        self.expect_finalized([lp1b])

    def test_run_cb_for_all_threads(self):
        self.start_transaction()
        lp1 = stm_allocate_with_finalizer(48)
        print lp1
        #
        self.switch(1)
        self.start_transaction()
        lp2 = stm_allocate_with_finalizer(56)
        print lp2

        self.expect_finalized([])
        stm_major_collect()
        self.switch(0)
        self.expect_finalized(2)

    def test_run_major_collect_in_finalizer(self):
        self.run_major_collect_in_finalizer = True
        self.start_transaction()
        lp1 = stm_allocate_with_finalizer(32)
        lp2 = stm_allocate_with_finalizer(32)
        lp3 = stm_allocate_with_finalizer(32)
        print lp1, lp2, lp3
        stm_major_collect()

    def test_new_objects_w_finalizers(self):
        self.switch(2)
        self.start_transaction()
        lp1 = stm_allocate_with_finalizer_refs(3)
        lp2 = stm_allocate_with_finalizer_refs(3)
        stm_set_ref(lp1, 0, lp2)

        self.push_root(lp1)
        stm_minor_collect()
        lp1 = self.pop_root()
        lp2 = stm_get_ref(lp1, 0)
        # lp1, lp2 have WB_EXECUTED objs

        self.expect_finalized([])
        stm_major_collect()
        self.expect_finalized([lp1])
