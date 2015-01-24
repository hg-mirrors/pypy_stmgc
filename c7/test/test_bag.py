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
