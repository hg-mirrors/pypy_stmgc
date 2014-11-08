from support import *
import random
import py, sys


def htget(o, key):
    h = get_hashtable(o)
    res = lib._check_hashtable_read(o, h, key)
    if res:
        raise Conflict
    return lib.hashtable_read_result

def htset(o, key, nvalue, tl):
    h = get_hashtable(o)
    res = lib._check_hashtable_write(o, h, key, nvalue, tl)
    if res:
        raise Conflict


class BaseTestHashtable(BaseTest):

    def setup_method(self, meth):
        BaseTest.setup_method(self, meth)
        #
        @ffi.callback("void(object_t *)")
        def light_finalizer(obj):
            print 'light_finalizer:', obj
            try:
                assert lib._get_type_id(obj) == 421419
                self.seen_hashtables -= 1
            except:
                self.errors.append(sys.exc_info()[2])
                raise

        lib.stmcb_light_finalizer = light_finalizer
        self._light_finalizer_keepalive = light_finalizer
        self.seen_hashtables = 0
        self.errors = []

    def teardown_method(self, meth):
        BaseTest.teardown_method(self, meth)
        lib.stmcb_light_finalizer = ffi.NULL
        assert self.errors == []
        assert self.seen_hashtables == 0

    def allocate_hashtable(self):
        h = stm_allocate_hashtable()
        lib.stm_enable_light_finalizer(h)
        self.seen_hashtables += 1
        return h


class TestHashtable(BaseTestHashtable):

    def test_empty(self):
        self.start_transaction()
        h = self.allocate_hashtable()
        for i in range(100):
            index = random.randrange(0, 1<<64)
            got = htget(h, index)
            assert got == ffi.NULL

    def test_set_value(self):
        self.start_transaction()
        tl0 = self.tls[self.current_thread]
        h = self.allocate_hashtable()
        lp1 = stm_allocate(16)
        htset(h, 12345678901, lp1, tl0)
        assert htget(h, 12345678901) == lp1
        for i in range(64):
            index = 12345678901 ^ (1 << i)
            assert htget(h, index) == ffi.NULL
        assert htget(h, 12345678901) == lp1

    def test_no_conflict(self):
        lp1 = stm_allocate_old(16)
        lp2 = stm_allocate_old(16)
        #
        self.start_transaction()
        tl0 = self.tls[self.current_thread]
        h = self.allocate_hashtable()
        self.push_root(h)
        stm_set_char(lp1, 'A')
        htset(h, 1234, lp1, tl0)
        self.commit_transaction()
        #
        self.start_transaction()
        h = self.pop_root()
        stm_set_char(lp2, 'B')
        htset(h, 9991234, lp2, tl0)
        #
        self.switch(1)
        self.start_transaction()
        lp1b = htget(h, 1234)
        assert lp1b != ffi.NULL
        assert stm_get_char(lp1b) == 'A'
        assert lp1b == lp1
        self.commit_transaction()
        #
        self.switch(0)
        assert htget(h, 9991234) == lp2
        assert stm_get_char(lp2) == 'B'
        assert htget(h, 1234) == lp1
        htset(h, 1234, ffi.NULL, tl0)
        self.commit_transaction()
        #
        self.start_transaction()
        stm_major_collect()       # to get rid of the hashtable object

    def test_conflict(self):
        lp1 = stm_allocate_old(16)
        lp2 = stm_allocate_old(16)
        #
        self.start_transaction()
        h = self.allocate_hashtable()
        self.push_root(h)
        self.commit_transaction()
        #
        self.start_transaction()
        h = self.pop_root()
        self.push_root(h)
        tl0 = self.tls[self.current_thread]
        htset(h, 1234, lp1, tl0)
        #
        self.switch(1)
        self.start_transaction()
        tl1 = self.tls[self.current_thread]
        py.test.raises(Conflict, "htset(h, 1234, lp2, tl1)")
        #
        self.switch(0)
        self.pop_root()
        stm_major_collect()       # to get rid of the hashtable object
        self.commit_transaction()

    def test_keepalive_minor(self):
        self.start_transaction()
        h = self.allocate_hashtable()
        self.push_root(h)
        lp1 = stm_allocate(16)
        stm_set_char(lp1, 'N')
        tl0 = self.tls[self.current_thread]
        htset(h, 1234, lp1, tl0)
        stm_minor_collect()
        h = self.pop_root()
        lp1b = htget(h, 1234)
        assert lp1b != ffi.NULL
        assert stm_get_char(lp1b) == 'N'
        assert lp1b != lp1

    def test_keepalive_major(self):
        lp1 = stm_allocate_old(16)
        #
        self.start_transaction()
        h = self.allocate_hashtable()
        self.push_root(h)
        stm_set_char(lp1, 'N')
        tl0 = self.tls[self.current_thread]
        htset(h, 1234, lp1, tl0)
        self.commit_transaction()
        #
        self.start_transaction()
        stm_major_collect()
        h = self.pop_root()
        lp1b = htget(h, 1234)
        assert lp1b == lp1
        assert stm_get_char(lp1b) == 'N'
        #
        stm_major_collect()       # to get rid of the hashtable object
        self.commit_transaction()

    def test_minor_collect_bug1(self):
        self.start_transaction()
        lp1 = stm_allocate(32)
        self.push_root(lp1)
        h = self.allocate_hashtable()
        self.push_root(h)
        stm_minor_collect()
        h = self.pop_root()
        lp1 = self.pop_root()
        print 'h', h                       # 0xa040010
        print 'lp1', lp1                   # 0xa040040
        tl0 = self.tls[self.current_thread]
        htset(h, 1, lp1, tl0)
        self.commit_transaction()
        #
        self.start_transaction()
        assert htget(h, 1) == lp1
        stm_major_collect()       # to get rid of the hashtable object

    def test_minor_collect_bug1_different_thread(self):
        self.start_transaction()
        lp1 = stm_allocate(32)
        self.push_root(lp1)
        h = self.allocate_hashtable()
        self.push_root(h)
        stm_minor_collect()
        h = self.pop_root()
        lp1 = self.pop_root()
        print 'h', h                       # 0xa040010
        print 'lp1', lp1                   # 0xa040040
        tl0 = self.tls[self.current_thread]
        htset(h, 1, lp1, tl0)
        self.commit_transaction()
        #
        self.switch(1)            # in a different thread
        self.start_transaction()
        assert htget(h, 1) == lp1
        stm_major_collect()       # to get rid of the hashtable object


class TestRandomHashtable(BaseTestHashtable):

    def setup_method(self, meth):
        BaseTestHashtable.setup_method(self, meth)
        self.values = []
        self.mirror = None
        self.roots = []
        self.other_thread = ([], [])

    def push_roots(self):
        assert self.roots is None
        self.roots = []
        for k, hitems in self.mirror.items():
            assert lib._get_type_id(k) == 421419
            for key, value in hitems.items():
                assert lib._get_type_id(value) < 1000
                self.push_root(value)
                self.roots.append(key)
            self.push_root(k)
            self.roots.append(None)
        for v in self.values:
            self.push_root(v)
        self.mirror = None

    def pop_roots(self):
        assert self.mirror is None
        for i in reversed(range(len(self.values))):
            self.values[i] = self.pop_root()
            assert stm_get_char(self.values[i]) == chr((i + 1) & 255)
        self.mirror = {}
        for r in reversed(self.roots):
            obj = self.pop_root()
            if r is None:
                assert lib._get_type_id(obj) == 421419
                self.mirror[obj] = curhitems = {}
            else:
                assert lib._get_type_id(obj) < 1000
                curhitems[r] = obj
        self.roots = None

    def exchange_threads(self):
        old_thread = (self.values, self.roots)
        self.switch(1 - self.current_thread)
        (self.values, self.roots) = self.other_thread
        self.mirror = None
        self.other_thread = old_thread

    def test_random_single_thread(self):
        import random
        #
        for i in range(100):
            print "start_transaction"
            self.start_transaction()
            self.pop_roots()
            for j in range(10):
                r = random.random()
                if r < 0.05:
                    h = self.allocate_hashtable()
                    print "allocate_hashtable ->", h
                    self.mirror[h] = {}
                elif r < 0.10:
                    print "stm_minor_collect"
                    self.push_roots()
                    stm_minor_collect()
                    self.pop_roots()
                elif r < 0.11:
                    print "stm_major_collect"
                    self.push_roots()
                    stm_major_collect()
                    self.pop_roots()
                elif r < 0.5:
                    if not self.mirror: continue
                    h = random.choice(self.mirror.keys())
                    if not self.mirror[h]: continue
                    key = random.choice(self.mirror[h].keys())
                    value = self.mirror[h][key]
                    print "htget(%r, %r) == %r" % (h, key, value)
                    self.push_roots()
                    self.push_root(value)
                    result = htget(h, key)
                    value = self.pop_root()
                    assert result == value
                    self.pop_roots()
                elif r < 0.6:
                    if not self.mirror: continue
                    h = random.choice(self.mirror.keys())
                    key = random.randrange(0, 40)
                    if key in self.mirror[h]: continue
                    print "htget(%r, %r) == NULL" % (h, key)
                    self.push_roots()
                    assert htget(h, key) == ffi.NULL
                    self.pop_roots()
                elif r < 0.63:
                    if not self.mirror: continue
                    h, _ = self.mirror.popitem()
                    print "popped", h
                elif r < 0.75:
                    obj = stm_allocate(32)
                    self.values.append(obj)
                    stm_set_char(obj, chr(len(self.values) & 255))
                else:
                    if not self.mirror or not self.values: continue
                    h = random.choice(self.mirror.keys())
                    key = random.randrange(0, 32)
                    value = random.choice(self.values)
                    print "htset(%r, %r, %r)" % (h, key, value)
                    self.push_roots()
                    tl = self.tls[self.current_thread]
                    htset(h, key, value, tl)
                    self.pop_roots()
                    self.mirror[h][key] = value
            self.push_roots()
            print "commit_transaction"
            self.commit_transaction()
        #
        self.start_transaction()
        self.become_inevitable()
        self.pop_roots()
        stm_major_collect()       # to get rid of the hashtable objects

    def test_random_multiple_threads(self):
        import random
        self.start_transaction()
        self.exchange_threads()
        self.start_transaction()
        self.pop_roots()
        #
        for j in range(1000):
            r = random.random()
            if r > 0.9:
                if r > 0.95:
                    self.push_roots()
                    self.commit_transaction()
                    self.start_transaction()
                    self.pop_roots()
                else:
                    self.push_roots()
                    self.exchange_threads()
                    self.pop_roots()
                continue

            if r < 0.05:
                h = self.allocate_hashtable()
                print "allocate_hashtable ->", h
                self.mirror[h] = {}
            elif r < 0.10:
                print "stm_minor_collect"
                self.push_roots()
                stm_minor_collect()
                self.pop_roots()
            elif r < 0.11:
                print "stm_major_collect"
                self.push_roots()
                stm_major_collect()
                self.pop_roots()
            elif r < 0.5:
                if not self.mirror: continue
                h = random.choice(self.mirror.keys())
                if not self.mirror[h]: continue
                key = random.choice(self.mirror[h].keys())
                value = self.mirror[h][key]
                print "htget(%r, %r) == %r" % (h, key, value)
                self.push_roots()
                self.push_root(value)
                result = htget(h, key)
                value = self.pop_root()
                assert result == value
                self.pop_roots()
            elif r < 0.6:
                if not self.mirror: continue
                h = random.choice(self.mirror.keys())
                key = random.randrange(0, 40)
                if key in self.mirror[h]: continue
                print "htget(%r, %r) == NULL" % (h, key)
                self.push_roots()
                assert htget(h, key) == ffi.NULL
                self.pop_roots()
            elif r < 0.63:
                if not self.mirror: continue
                h, _ = self.mirror.popitem()
                print "popped", h
            elif r < 0.75:
                obj = stm_allocate(32)
                self.values.append(obj)
                stm_set_char(obj, chr(len(self.values) & 255))
            else:
                if not self.mirror or not self.values: continue
                h = random.choice(self.mirror.keys())
                key = random.randrange(0, 32)
                value = random.choice(self.values)
                print "htset(%r, %r, %r)" % (h, key, value)
                self.push_roots()
                tl = self.tls[self.current_thread]
                htset(h, key, value, tl)
                self.pop_roots()
                self.mirror[h][key] = value
        #
        print 'closing down...'
        self.become_inevitable()
        self.commit_transaction()
        self.exchange_threads()
        self.pop_roots()
        self.become_inevitable()
        stm_major_collect()       # to get rid of the hashtable objects
