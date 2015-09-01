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

def ht_length_upper_bound(o):
    h = get_hashtable(o)
    return lib.stm_hashtable_length_upper_bound(h)

def htitems(o):
    h = get_hashtable(o)
    upper_bound = lib.stm_hashtable_length_upper_bound(h)
    entries = stm_allocate_refs(upper_bound)
    count = lib._stm_hashtable_list(o, h, entries)
    assert count <= upper_bound

    return [(lib._get_entry_index(ffi.cast("stm_hashtable_entry_t *", stm_get_ref(entries, i))),
             lib._get_entry_object(ffi.cast("stm_hashtable_entry_t *", stm_get_ref(entries, i))))
            for i in range(count)]

def htlen(o):
    h = get_hashtable(o)
    count = lib._stm_hashtable_list(o, h, ffi.NULL)
    return count


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
                h = get_hashtable(obj)
                lib.stm_hashtable_free(h)
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
        self.switch(1)
        self.start_transaction()
        self.switch(0)
        htset(h, 9991234, lp2, tl0)
        #
        self.switch(1)
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
        htset(h, 1234, lp2, tl1)
        #
        self.switch(0)
        self.commit_transaction()
        #
        py.test.raises(Conflict, self.switch, 1)
        #
        self.switch(0)
        self.start_transaction()
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

    def test_major_collect_bug2(self):
        self.start_transaction()
        lp1 = stm_allocate(24)
        self.push_root(lp1)
        self.commit_transaction()
        lp1 = self.pop_root()
        #
        self.switch(1)
        self.start_transaction()
        stm_write(lp1)            # force this page to be shared
        #
        self.switch(0)
        self.start_transaction()
        h = self.allocate_hashtable()
        tl0 = self.tls[self.current_thread]
        htset(h, 10, stm_allocate(32), tl0)
        htset(h, 11, stm_allocate(32), tl0)
        htset(h, 12, stm_allocate(32), tl0)
        self.push_root(h)
        #
        self.switch(1)            # in a different thread
        stm_major_collect()       # force a _stm_rehash_hashtable()
        #
        self.switch(0)            # back to the original thread
        h = self.pop_root()
        assert htget(h, 10) != ffi.NULL
        assert htget(h, 11) != ffi.NULL
        assert htget(h, 12) != ffi.NULL

    def test_list_1(self):
        self.start_transaction()
        h = self.allocate_hashtable()
        tl0 = self.tls[self.current_thread]
        for i in range(32):
            assert ht_length_upper_bound(h) == i
            htset(h, 19 ^ i, stm_allocate(32), tl0)
        assert ht_length_upper_bound(h) == 32

    def test_list_2(self):
        self.start_transaction()
        h = self.allocate_hashtable()
        tl0 = self.tls[self.current_thread]
        expected = []
        for i in range(29):
            lp = stm_allocate(32)
            htset(h, 19 ^ i, lp, tl0)
            expected.append((19 ^ i, lp))
        lst = htitems(h)
        assert len(lst) == 29
        assert sorted(lst) == sorted(expected)

    def test_list_3(self):
        self.start_transaction()
        h = self.allocate_hashtable()
        tl0 = self.tls[self.current_thread]
        for i in range(29):
            htset(h, 19 ^ i, stm_allocate(32), tl0)
        assert htlen(h) == 29

    def test_len_conflicts_with_additions(self):
        self.start_transaction()
        h = self.allocate_hashtable()
        self.push_root(h)
        self.commit_transaction()
        #
        self.start_transaction()
        h = self.pop_root()
        self.push_root(h)
        tl0 = self.tls[self.current_thread]
        htset(h, 10, stm_allocate(32), tl0)
        #
        self.switch(1)
        self.start_transaction()
        assert htlen(h) == 0
        #
        self.switch(0)
        self.commit_transaction()
        #
        py.test.raises(Conflict, self.switch, 1)
        #
        self.switch(0)
        self.start_transaction()
        self.pop_root()
        stm_major_collect()       # to get rid of the hashtable object
        self.commit_transaction()

    def test_grow_without_conflict(self):
        self.start_transaction()
        h = self.allocate_hashtable()
        self.push_root(h)
        self.commit_transaction()
        h = self.pop_root()
        self.push_root(h)
        #
        STEPS = 50
        for i in range(STEPS):
            self.switch(1)
            self.start_transaction()
            tl0 = self.tls[self.current_thread]
            htset(h, i + STEPS, stm_allocate(32), tl0)
            #
            self.switch(0)
            self.start_transaction()
            tl0 = self.tls[self.current_thread]
            htset(h, i, stm_allocate(24), tl0)
            #
            self.switch(1)
            self.commit_transaction()
            #
            self.switch(0)
            self.commit_transaction()
        #
        self.pop_root()
        self.start_transaction()
        stm_major_collect()       # to get rid of the hashtable object


    def test_new_entry_if_nursery_full(self):
        self.start_transaction()
        tl0 = self.tls[self.current_thread]
        # make sure we fill the nursery *exactly* so that
        # the last entry allocation triggers a minor GC
        # and needs to allocate preexisting outside the nursery:
        SMALL = 24 + lib.SIZEOF_MYOBJ
        assert (NURSERY_SIZE - SIZEOF_HASHTABLE_OBJ) % SMALL < SIZEOF_HASHTABLE_ENTRY
        to_alloc = (NURSERY_SIZE - SIZEOF_HASHTABLE_OBJ) // SMALL
        for i in range(to_alloc):
            stm_allocate(SMALL)
        h = self.allocate_hashtable()
        assert is_in_nursery(h)
        self.push_root(h)
        # would trigger minor GC when allocating 'entry' in nursery:
        entry = hashtable_lookup(h, get_hashtable(h), 123)
        h = self.pop_root()
        self.push_root(h)
        assert is_in_nursery(h) # didn't trigger minor-gc, since entry allocated outside
        assert not is_in_nursery(entry)
        assert htget(h, 123) == ffi.NULL
        htset(h, 123, h, tl0)

        # stm_write(h) - the whole thing may be fixed also by ensuring
        # the hashtable gets retraced in minor-GC if stm_hashtable_write_entry
        # detects the 'entry' to be young (and hobj being old)

        stm_minor_collect()
        h = self.pop_root()
        assert htget(h, 123) == h
        entry2 = hashtable_lookup(h, get_hashtable(h), 123)
        assert entry == entry2
        assert not is_in_nursery(h)
        assert not is_in_nursery(entry2)

        # get rid of ht:
        self.commit_transaction()
        self.start_transaction()
        stm_major_collect()
        self.commit_transaction()




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
        print "----- switch to %s -----" % (self.current_thread,)

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
        from random import randrange, Random
        seed = randrange(0, 10000)
        print "----------------------------------------- seed:", seed
        random = Random(seed)
        #
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
                print "allocate_hashtable -> %r/%r" % (h, get_hashtable(h))
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
                print "htget(%r/%r, %r) == %r" % (h, get_hashtable(h), key, value)
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
                print "htget(%r/%r, %r) == NULL" % (h, get_hashtable(h), key)
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
                print "htset(%r/%r, %r, %r)" % (h, get_hashtable(h), key, value)
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
