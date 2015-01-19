from support import *
import py


class TestLightFinalizer(BaseTest):

    def setup_method(self, meth):
        BaseTest.setup_method(self, meth)
        #
        @ffi.callback("void(object_t *)")
        def light_finalizer(obj):
            segnum = lib.current_segment_num()
            tlnum = '?'
            for n, tl in enumerate(self.tls):
                if tl.associated_segment_num == segnum:
                    tlnum = n
                    break
            self.light_finalizers_called.append((obj, tlnum))
        self.light_finalizers_called = []
        lib.stmcb_light_finalizer = light_finalizer
        self._light_finalizer_keepalive = light_finalizer

    def expect_finalized(self, objs, from_tlnum=None):
        assert [obj for (obj, tlnum) in self.light_finalizers_called] == objs
        if from_tlnum is not None:
            for obj, tlnum in self.light_finalizers_called:
                assert tlnum == from_tlnum
        self.light_finalizers_called = []

    def test_no_finalizer(self):
        self.start_transaction()
        lp1 = stm_allocate(48)
        self.commit_transaction()
        self.expect_finalized([])

    def test_young_light_finalizer(self):
        self.start_transaction()
        lp1 = stm_allocate(48)
        lib.stm_enable_light_finalizer(lp1)
        self.expect_finalized([])
        self.commit_transaction()
        self.expect_finalized([lp1], from_tlnum=0)

    def test_young_light_finalizer_survives(self):
        self.start_transaction()
        lp1 = stm_allocate(48)
        lib.stm_enable_light_finalizer(lp1)
        self.push_root(lp1)       # stays alive
        self.commit_transaction()
        self.expect_finalized([])

    def test_old_light_finalizer(self):
        self.start_transaction()
        lp1 = stm_allocate(48)
        self.push_root(lp1)
        stm_minor_collect()
        lp1 = self.pop_root()
        lib.stm_enable_light_finalizer(lp1)
        self.commit_transaction()
        self.expect_finalized([])

    def test_old_light_finalizer_2(self):
        self.start_transaction()
        lp1 = stm_allocate(48)
        lib.stm_enable_light_finalizer(lp1)
        self.push_root(lp1)
        stm_minor_collect()
        lp1 = self.pop_root()
        self.expect_finalized([])
        stm_major_collect()
        self.expect_finalized([lp1])
        self.commit_transaction()

    def test_old_light_finalizer_survives(self):
        self.start_transaction()
        lp1 = stm_allocate(48)
        lib.stm_enable_light_finalizer(lp1)
        self.push_root(lp1)
        stm_minor_collect()
        lp1 = self.pop_root()
        self.push_root(lp1)
        stm_major_collect()
        self.commit_transaction()
        self.expect_finalized([])

    def test_old_light_finalizer_segment(self):
        self.start_transaction()
        #
        self.switch(1)
        self.start_transaction()
        lp1 = stm_allocate(48)
        lib.stm_enable_light_finalizer(lp1)
        self.push_root(lp1)
        stm_minor_collect()
        lp1 = self.pop_root()
        #
        self.switch(0)
        self.expect_finalized([])
        stm_major_collect()
        self.expect_finalized([lp1], from_tlnum=1)


class TestRegularFinalizer(BaseTest):

    def setup_method(self, meth):
        BaseTest.setup_method(self, meth)
        #
        @ffi.callback("void(object_t *)")
        def finalizer(obj):
            self.finalizers_called.append(obj)
        self.finalizers_called = []
        lib.stmcb_finalizer = finalizer
        self._finalizer_keepalive = finalizer

    def expect_finalized(self, objs):
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
        lp1 = stm_allocate_with_finalizer(48)
        lp2 = stm_allocate_with_finalizer(48)
        lp3 = stm_allocate_with_finalizer(48)
        print lp1, lp2, lp3
        stm_major_collect()
        self.expect_finalized([lp1, lp2, lp3])

    def test_finalizer_ordering(self):
        self.start_transaction()
        lp1 = stm_allocate_with_finalizer_refs(1)
        lp2 = stm_allocate_with_finalizer_refs(1)
        lp3 = stm_allocate_with_finalizer_refs(1)
        print lp1, lp2, lp3
        stm_set_ref(lp3, 0, lp1)
        stm_set_ref(lp1, 0, lp2)
        stm_major_collect()
        self.expect_finalized([lp3])

    def test_finalizer_extra_transation(self):
        self.start_transaction()
        lp1 = stm_allocate_with_finalizer(32)
        print lp1
        self.push_root(lp1)
        self.commit_transaction()

        self.start_transaction()
        lp1b = self.pop_root()
        assert lp1b == lp1
        self.expect_finalized([])
        self.commit_transaction()
        self.expect_finalized([])

        self.start_transaction()
        stm_major_collect()
        self.expect_finalized([])
        self.commit_transaction()
        self.expect_finalized([lp1])

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
        self.expect_finalized([lp2, lp1])
