from support import *
import py


class TestFinalizer(BaseTest):

    def setup_method(self, meth):
        BaseTest.setup_method(self, meth)
        #
        @ffi.callback("void(object_t *)")
        def light_finalizer(obj):
            self.light_finalizers_called.append(obj)
        self.light_finalizers_called = []
        lib.stmcb_light_finalizer = light_finalizer
        self._light_finalizer_keepalive = light_finalizer

    def expect_finalized(self, objs):
        assert self.light_finalizers_called == objs
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
        self.expect_finalized([lp1])

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

    def test_old_light_finalizer(self):
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
