from support import *
import random
import py, sys


class Empty(Exception):
    pass


class BaseTestQueue(BaseTest):

    def setup_method(self, meth):
        BaseTest.setup_method(self, meth)
        #
        @ffi.callback("void(object_t *)")
        def light_finalizer(obj):
            print 'light_finalizer:', obj
            try:
                assert lib._get_type_id(obj) == 421417
                self.seen_queues -= 1
            except:
                self.errors.append(sys.exc_info()[2])
                raise

        lib.stmcb_light_finalizer = light_finalizer
        self._light_finalizer_keepalive = light_finalizer
        self.seen_queues = 0
        self.errors = []

    def teardown_method(self, meth):
        BaseTest.teardown_method(self, meth)
        lib.stmcb_light_finalizer = ffi.NULL
        assert self.errors == []
        assert self.seen_queues == 0

    def allocate_queue(self):
        q = stm_allocate_queue()
        lib.stm_enable_light_finalizer(q)
        self.seen_queues += 1
        return q

    def get(self, obj):
        q = lib._get_queue(obj)
        res = lib.stm_queue_get(obj, q, self.tls[self.current_thread])
        if res == ffi.cast("object_t *", 42):
            raise Empty
        return res

    def put(self, obj, newitem):
        q = lib._get_queue(obj)
        lib.stm_queue_put(q, newitem)


class TestQueue(BaseTestQueue):

    def test_empty(self):
        self.start_transaction()
        qobj = self.allocate_queue()
        py.test.raises(Empty, self.get, qobj)

    def test_put_get_same_transaction(self):
        self.start_transaction()
        obj1 = stm_allocate(32)
        obj2 = stm_allocate(32)
        qobj = self.allocate_queue()
        print "put"
        self.put(qobj, obj1)
        self.put(qobj, obj1)
        self.put(qobj, obj2)
        print "done"
        got = {}
        for i in range(3):
            o = self.get(qobj)
            got[o] = got.get(o, 0) + 1
        assert got == {obj1: 2, obj2: 1}
