from support import *
import py


class BagLooksEmpty(Exception):
    pass

def b_add(o, nvalue):
    b = get_bag(o)
    lib.stm_bag_add(b, nvalue)

def b_pop(o):
    b = get_bag(o)
    r = lib.stm_bag_try_pop(b)
    if not r:
        raise BagLooksEmpty
    return r


class BaseTestBag(BaseTest):

    def setup_method(self, meth):
        BaseTest.setup_method(self, meth)
        #
        @ffi.callback("void(object_t *)")
        def light_finalizer(obj):
            print 'light_finalizer:', obj
            try:
                assert lib._get_type_id(obj) == 421417
                self.seen_bags -= 1
            except:
                self.errors.append(sys.exc_info()[2])
                raise

        lib.stmcb_light_finalizer = light_finalizer
        self._light_finalizer_keepalive = light_finalizer
        self.seen_bags = 0
        self.errors = []

    def teardown_method(self, meth):
        BaseTest.teardown_method(self, meth)
        lib.stmcb_light_finalizer = ffi.NULL
        assert self.errors == []
        assert self.seen_bags == 0

    def allocate_bag(self):
        q = stm_allocate_bag()
        lib.stm_enable_light_finalizer(q)
        self.seen_bags += 1
        return q


class TestBag(BaseTestBag):

    def test_small_push_pop(self):
        self.start_transaction()
        q = self.allocate_bag()
        lp1 = stm_allocate(16)
        lp2 = stm_allocate(16)
        for i in range(4):
            b_add(q, lp1)
            b_add(q, lp2)
        for j in range(4):
            got = b_pop(q)
            assert got == lp1
            got = b_pop(q)
            assert got == lp2
        py.test.raises(BagLooksEmpty, b_pop, q)
        py.test.raises(BagLooksEmpty, b_pop, q)

    def test_large_push_pop(self):
        self.start_transaction()
        q = self.allocate_bag()
        lps = [stm_allocate(16) for i in range(65)]
        for lp in lps:
            b_add(q, lp)
        for lp in lps:
            got = b_pop(q)
            assert got == lp
        py.test.raises(BagLooksEmpty, b_pop, q)
        py.test.raises(BagLooksEmpty, b_pop, q)

    def test_keepalive_minor(self):
        self.start_transaction()
        b = self.allocate_bag()
        self.push_root(b)
        lp1 = stm_allocate(16)
        stm_set_char(lp1, 'N')
        b_add(b, lp1)
        stm_minor_collect()
        b = self.pop_root()
        lp1b = b_pop(b)
        assert lp1b != ffi.NULL
        assert stm_get_char(lp1b) == 'N'
        assert lp1b != lp1
        #
        lp2 = stm_allocate(16)
        stm_set_char(lp2, 'M')
        b_add(b, lp2)
        stm_minor_collect()
        lp2b = b_pop(b)
        assert lp2b != ffi.NULL
        assert stm_get_char(lp2b) == 'M'
        assert lp2b != lp2

    def test_keepalive_major(self):
        self.start_transaction()
        b = self.allocate_bag()
        self.push_root(b)
        lp1 = stm_allocate(16)
        stm_set_char(lp1, 'N')
        b_add(b, lp1)
        stm_major_collect()
        b = self.pop_root()
        lp1b = b_pop(b)
        assert lp1b != ffi.NULL
        assert stm_get_char(lp1b) == 'N'
        assert lp1b != lp1

    def test_transaction_local(self):
        self.start_transaction()
        q = self.allocate_bag()
        self.push_root(q)
        self.commit_transaction()
        q = self.pop_root()
        #
        self.start_transaction()
        lp1 = stm_allocate(16)
        b_add(q, lp1)
        #
        self.switch(1)
        self.start_transaction()
        lp2 = stm_allocate(16)
        b_add(q, lp2)
        got = b_pop(q)
        assert got == lp2
        #
        self.switch(0)
        got = b_pop(q)
        assert got == lp1
        #
        stm_major_collect()       # to get rid of the bag object

    def test_abort_recovers_popped(self):
        self.start_transaction()
        q = self.allocate_bag()
        self.push_root(q)
        lp1 = stm_allocate(16)
        lp2 = stm_allocate(16)
        stm_set_char(lp1, 'M')
        stm_set_char(lp2, 'N')
        b_add(q, lp1)
        b_add(q, lp2)
        self.commit_transaction()
        q = self.pop_root()
        #
        self.start_transaction()
        lp1 = b_pop(q)
        lp2 = b_pop(q)
        assert stm_get_char(lp1) == 'M'
        assert stm_get_char(lp2) == 'N'
        self.abort_transaction()
        #
        self.start_transaction()
        lp1 = b_pop(q)
        lp2 = b_pop(q)
        assert stm_get_char(lp1) == 'M'
        assert stm_get_char(lp2) == 'N'
        #
        stm_major_collect()       # to get rid of the bag object
