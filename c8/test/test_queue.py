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
                q = get_queue(obj)
                lib.stm_queue_free(q)
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
        q = get_queue(obj)
        res = lib.stm_queue_get(obj, q, 0.0, self.tls[self.current_thread])
        if res == ffi.NULL:
            raise Empty
        return res

    def put(self, obj, newitem):
        q = get_queue(obj)
        lib.stm_queue_put(obj, q, newitem)

    def task_done(self, obj):
        q = get_queue(obj)
        lib.stm_queue_task_done(q)

    def join(self, obj):
        q = get_queue(obj)
        res = lib.stm_queue_join(obj, q, self.tls[self.current_thread]);
        if res == 1:
            return
        elif res == 42:
            raise Conflict("join() cannot wait in tests")
        else:
            raise AssertionError("stm_queue_join error")


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
        #
        assert not self.is_inevitable()
        py.test.raises(Empty, self.get, qobj)
        assert self.is_inevitable()
        py.test.raises(Empty, self.get, qobj)
        assert self.is_inevitable()

    def test_put_get_next_transaction(self):
        self.start_transaction()
        obj1 = stm_allocate(32)
        obj2 = stm_allocate(32)
        stm_set_char(obj1, 'D')
        stm_set_char(obj2, 'F')
        qobj = self.allocate_queue()
        self.push_root(qobj)
        print "put"
        self.put(qobj, obj1)
        self.put(qobj, obj1)
        self.put(qobj, obj2)
        print "done"
        self.commit_transaction()
        #
        self.start_transaction()
        qobj = self.pop_root()
        got = {}
        for i in range(3):
            o = self.get(qobj)
            c = stm_get_char(o)
            got[c] = got.get(c, 0) + 1
        print got
        assert got == {'D': 2, 'F': 1}
        self.commit_transaction()
        #
        self.start_transaction()
        stm_major_collect()       # to get rid of the queue object

    def test_get_along_several_transactions(self):
        self.start_transaction()
        obj1 = stm_allocate(32)
        obj2 = stm_allocate(32)
        stm_set_char(obj1, 'D')
        stm_set_char(obj2, 'F')
        qobj = self.allocate_queue()
        self.push_root(qobj)
        self.put(qobj, obj1)
        self.put(qobj, obj1)
        self.put(qobj, obj2)
        self.commit_transaction()
        qobj = self.pop_root()
        #
        got = {}
        for i in range(3):
            self.start_transaction()
            o = self.get(qobj)
            c = stm_get_char(o)
            got[c] = got.get(c, 0) + 1
            self.commit_transaction()
        #
        print got
        assert got == {'D': 2, 'F': 1}
        #
        self.start_transaction()
        stm_major_collect()       # to get rid of the queue object

    def test_reenable_minor_collections(self):
        self.start_transaction()
        qobj = self.allocate_queue()
        #
        self.push_root(qobj)
        obj1 = stm_allocate(32)
        stm_set_char(obj1, 'G')
        self.put(qobj, obj1)
        stm_minor_collect()
        qobj = self.pop_root()
        obj1 = self.get(qobj)
        assert stm_get_char(obj1) == 'G'
        #
        obj2 = stm_allocate(32)
        stm_set_char(obj2, 'H')
        self.put(qobj, obj2)
        stm_minor_collect()
        obj2 = self.get(qobj)
        assert stm_get_char(obj2) == 'H'
        #
        stm_major_collect()       # to get rid of the queue object

    def test_parallel_transactions(self):
        self.start_transaction()
        qobj = self.allocate_queue()
        self.push_root(qobj)
        self.commit_transaction()
        qobj = self.pop_root()
        #
        self.start_transaction()
        obj1 = stm_allocate(32)
        stm_set_char(obj1, 'U')
        self.put(qobj, obj1)
        #
        self.switch(1)
        self.start_transaction()
        py.test.raises(Empty, self.get, qobj)
        self.commit_transaction()
        self.start_transaction()
        #
        self.switch(0)
        self.commit_transaction()
        #
        self.switch(1)
        obj1 = self.get(qobj)
        assert stm_get_char(obj1) == 'U'
        #
        stm_major_collect()       # to get rid of the queue object

    def test_abort_restores_items(self):
        self.start_transaction()
        qobj = self.allocate_queue()
        #
        self.push_root(qobj)
        obj1 = stm_allocate(32)
        stm_set_char(obj1, 'G')
        self.put(qobj, obj1)
        self.commit_transaction()
        qobj = self.pop_root()
        #
        self.start_transaction()
        self.get(qobj)
        self.abort_transaction()
        #
        self.start_transaction()
        obj1 = self.get(qobj)
        assert stm_get_char(obj1) == 'G'
        #
        stm_major_collect()       # to get rid of the queue object

    def test_major_collection_added_in_this_transaction(self):
        self.start_transaction()
        qobj = self.allocate_queue()
        #
        self.push_root(qobj)
        obj1 = stm_allocate(32)
        stm_set_char(obj1, 'G')
        self.put(qobj, obj1)
        stm_major_collect()
        qobj = self.pop_root()
        obj1 = self.get(qobj)
        assert stm_get_char(obj1) == 'G'
        #
        stm_major_collect()       # to get rid of the queue object

    def test_major_collection_old_objects_popped(self):
        self.start_transaction()
        qobj = self.allocate_queue()
        #
        self.push_root(qobj)
        obj1 = stm_allocate(32)
        stm_set_char(obj1, 'G')
        self.put(qobj, obj1)
        self.commit_transaction()
        qobj = self.pop_root()
        #
        self.start_transaction()
        self.get(qobj)
        self.push_root(qobj)
        stm_major_collect()
        self.abort_transaction()
        #
        self.start_transaction()
        obj1 = self.get(qobj)
        assert stm_get_char(obj1) == 'G'
        #
        stm_major_collect()       # to get rid of the queue object

    def test_major_collection_old_entries(self):
        self.start_transaction()
        qobj = self.allocate_queue()
        #
        self.push_root(qobj)
        obj1 = stm_allocate(32)
        stm_set_char(obj1, 'G')
        self.put(qobj, obj1)
        self.commit_transaction()
        qobj = self.pop_root()
        #
        self.start_transaction()
        self.push_root(qobj)
        stm_major_collect()
        self.pop_root()
        obj1 = self.get(qobj)
        assert stm_get_char(obj1) == 'G'
        #
        stm_major_collect()       # to get rid of the queue object

    def test_add_with_minor_collection(self):
        self.start_transaction()
        qobj = self.allocate_queue()
        for i in range(10):
            print '.'
            obj1 = stm_allocate(16)
            stm_set_char(obj1, chr(i))
            print '+'
            self.put(qobj, obj1)
            self.push_root(qobj)
            stm_minor_collect()
            qobj = self.pop_root()
        #
        for i in reversed(range(10)):    # reverse order, in this case, for now
            obj1 = self.get(qobj)
            assert stm_get_char(obj1) == chr(i)
            stm_minor_collect()

    def test_add_complexity(self):
        py.test.skip("performance test")
        # on my laptop, runs in 43.8 sec using 'added_young_limit' and
        # in 56.2 sec without using 'added_young_limit' (most likely,
        # with quadratic complexity)
        self.start_transaction()
        qobj = self.allocate_queue()
        for i in range(40000):
            print '.'
            obj1 = stm_allocate(16)
            self.put(qobj, obj1)
            self.push_root(qobj)
            stm_minor_collect()
            qobj = self.pop_root()

    def test_task_done_1(self):
        self.start_transaction()
        qobj = self.allocate_queue()
        self.push_root(qobj)
        stm_minor_collect()
        qobj = self.pop_root()
        self.join(qobj)
        obj1 = stm_allocate(32)
        self.put(qobj, obj1)
        py.test.raises(Conflict, self.join, qobj)
        self.get(qobj)
        py.test.raises(Conflict, self.join, qobj)
        self.task_done(qobj)
        self.join(qobj)

    def test_task_done_2(self):
        self.start_transaction()
        qobj = self.allocate_queue()
        self.push_root(qobj)
        self.put(qobj, stm_allocate(32))
        self.put(qobj, stm_allocate(32))
        self.get(qobj)
        self.get(qobj)
        self.commit_transaction()
        qobj = self.pop_root()
        #
        self.start_transaction()
        py.test.raises(Conflict, self.join, qobj)
        #
        self.switch(1)
        self.start_transaction()
        py.test.raises(Conflict, self.join, qobj)
        self.task_done(qobj)
        py.test.raises(Conflict, self.join, qobj)
        self.task_done(qobj)
        self.join(qobj)
        #
        self.switch(0)
        py.test.raises(Conflict, self.join, qobj)
        #
        self.switch(1)
        self.commit_transaction()
        #
        self.switch(0)
        self.join(qobj)
        #
        stm_major_collect()       # to get rid of the queue object
