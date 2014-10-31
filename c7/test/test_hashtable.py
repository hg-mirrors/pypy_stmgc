from support import *
import random
import py


def htget(o, key):
    h = get_hashtable(o)
    res = lib._check_hashtable_read(o, h, key)
    if res:
        raise Conflict
    return lib.hashtable_read_result

def htset(o, key, nvalue):
    h = get_hashtable(o)
    res = lib._check_hashtable_write(o, h, key, nvalue)
    if res:
        raise Conflict


class TestHashtable(BaseTest):

    def test_empty(self):
        self.start_transaction()
        h = stm_allocate_hashtable()
        for i in range(100):
            index = random.randrange(0, 1<<64)
            got = htget(h, index)
            assert got == ffi.NULL

    def test_set_value(self):
        self.start_transaction()
        h = stm_allocate_hashtable()
        lp1 = stm_allocate(16)
        htset(h, 12345678901, lp1)
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
        h = stm_allocate_hashtable()
        self.push_root(h)
        stm_set_char(lp1, 'A')
        htset(h, 1234, lp1)
        self.commit_transaction()
        #
        self.start_transaction()
        h = self.pop_root()
        stm_set_char(lp2, 'B')
        htset(h, 9991234, lp2)
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
        htset(h, 1234, ffi.NULL)
        self.commit_transaction()

    def test_conflict(self):
        lp1 = stm_allocate_old(16)
        lp2 = stm_allocate_old(16)
        #
        self.start_transaction()
        h = stm_allocate_hashtable()
        self.push_root(h)
        self.commit_transaction()
        #
        self.start_transaction()
        h = self.pop_root()
        self.push_root(h)
        htset(h, 1234, lp1)
        #
        self.switch(1)
        self.start_transaction()
        py.test.raises(Conflict, "htset(h, 1234, lp2)")

    def test_keepalive_minor(self):
        self.start_transaction()
        h = stm_allocate_hashtable()
        self.push_root(h)
        lp1 = stm_allocate(16)
        stm_set_char(lp1, 'N')
        htset(h, 1234, lp1)
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
        h = stm_allocate_hashtable()
        self.push_root(h)
        stm_set_char(lp1, 'N')
        htset(h, 1234, lp1)
        self.commit_transaction()
        #
        self.start_transaction()
        stm_major_collect()
        h = self.pop_root()
        self.push_root(h)
        lp1b = htget(h, 1234)
        assert lp1b == lp1
        assert stm_get_char(lp1b) == 'N'
