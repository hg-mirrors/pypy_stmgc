from support import *
import py


class TestLightFinalizer(BaseTest):

    def setup_method(self, meth):
        BaseTest.setup_method(self, meth)
        #
        @ffi.callback("void(object_t *)")
        def light_finalizer(obj):
            assert stm_get_obj_size(obj) == 48
            segnum = lib.current_segment_num()
            tlnum = '?'
            for n, tl in enumerate(self.tls):
                if lib._stm_in_transaction(tl):
                    if tl.last_associated_segment_num == segnum:
                        tlnum = n
                        break
            self.light_finalizers_called.append((obj, tlnum))
        self.light_finalizers_called = []
        lib.stmcb_light_finalizer = light_finalizer
        self._light_finalizer_keepalive = light_finalizer

    def teardown_method(self, meth):
        lib.stmcb_light_finalizer = ffi.NULL
        BaseTest.teardown_method(self, meth)

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

    def test_young_light_finalizer_aborts(self):
        self.start_transaction()
        lp1 = stm_allocate(48)
        lib.stm_enable_light_finalizer(lp1)
        self.expect_finalized([])
        self.abort_transaction()
        self.start_transaction()
        self.expect_finalized([lp1], from_tlnum=0)

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

    def test_old_light_finalizer_aborts(self):
        self.start_transaction()
        lp1 = stm_allocate(48)
        lib.stm_enable_light_finalizer(lp1)
        self.push_root(lp1)
        self.commit_transaction()
        #
        self.start_transaction()
        self.expect_finalized([])
        self.abort_transaction()
        self.expect_finalized([])

    def test_overflow_light_finalizer_aborts(self):
        self.start_transaction()
        lp1 = stm_allocate(48)
        lib.stm_enable_light_finalizer(lp1)
        self.push_root(lp1)
        stm_minor_collect()
        lp1 = self.pop_root()
        self.push_root(lp1)
        self.expect_finalized([])
        self.abort_transaction()
        self.expect_finalized([lp1], from_tlnum=0)



class TestRegularFinalizer(BaseTest):
    expect_content_character = None
    run_major_collect_in_finalizer = False

    def setup_method(self, meth):
        BaseTest.setup_method(self, meth)
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
            print repeat, lp1, lp2, lp3
            self.expect_finalized([])
            stm_major_collect()
            self.expect_finalized([lp1, lp2, lp3])

    def test_finalizer_from_other_thread(self):
        self.start_transaction()
        lp1 = stm_allocate_with_finalizer(48)
        stm_set_char(lp1, 'H')
        self.expect_content_character = 'H'
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


class TestMoreRegularFinalizers(BaseTest):

    def test_inevitable_in_finalizer(self):
        lpo = stm_allocate_old(16)

        self._first_time = True
        @ffi.callback("void(object_t *)")
        def finalizer(obj):
            print "finalizing!", obj
            stm_set_char(lpo, 'a')

            if self._first_time:
                self._first_time = False
                # we will switch to the other TX and
                # make it inevitable, so that our TX
                # will abort on commit (or validate)
                self.switch(0, validate=False)
                self.become_inevitable()
                self.switch(1, validate=False)

        lib.stmcb_finalizer = finalizer
        self._finalizer_keepalive = finalizer

        # start a transaction with a finalizing obj
        self.switch(1)
        self.start_transaction()
        lpf = stm_allocate_with_finalizer(16)

        self.push_root(lpf)
        stm_minor_collect()


        self.switch(0)
        self.start_transaction()
        stm_set_char(lpo, 'x')
        self.switch(1)
        lpf = self.pop_root()
        # commit this TX, start a new one, let lpf
        # die with a major-gc:
        self.commit_transaction()
        self.start_transaction()
        stm_major_collect()
        # commit and run finalizer in separate TX
        # that will abort because of a conflict
        self.commit_transaction()

        self.switch(0, validate=False)
        assert stm_get_char(lpo) == 'x'
        # commit the now-inevitable TX and run
        # the aborted finalizer again
        self.commit_transaction()
        self.start_transaction()
        # should now see the value set by finalizer
        assert stm_get_char(lpo) == 'a'
