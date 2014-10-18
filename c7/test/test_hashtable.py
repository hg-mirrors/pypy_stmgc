from support import *
import random
import py


class StmHashTable(object):
    def __init__(self):
        self.h = lib.stm_hashtable_create()

    def free(self):
        lib.stm_hashtable_free(self.h)

    def __getitem__(self, key):
        res = lib._check_hashtable_read(self.h, key)
        if res:
            raise Conflict
        return lib.hashtable_read_result

    def __setitem__(self, key, nvalue):
        res = lib._check_hashtable_write(self.h, key, nvalue)
        if res:
            raise Conflict


class TestHashtable(BaseTest):

    def test_empty(self):
        self.start_transaction()
        h = StmHashTable()
        for i in range(100):
            index = random.randrange(0, 1<<64)
            got = h[index]
            assert got == ffi.NULL
        h.free()

    def test_set_value(self):
        self.start_transaction()
        h = StmHashTable()
        lp1 = stm_allocate(16)
        h[12345678901] = lp1
        assert h[12345678901] == lp1
        for i in range(64):
            index = 12345678901 ^ (1 << i)
            assert h[index] == ffi.NULL
        assert h[12345678901] == lp1
        h.free()

    def test_no_conflict(self):
        h = StmHashTable()
        lp1 = stm_allocate_old(16)
        lp2 = stm_allocate_old(16)
        #
        self.start_transaction()
        stm_set_char(lp1, 'A')
        h[1234] = lp1
        self.commit_transaction()
        #
        self.start_transaction()
        stm_set_char(lp2, 'B')
        h[9991234] = lp2
        #
        self.switch(1)
        self.start_transaction()
        lp1b = h[1234]
        assert stm_get_char(lp1b) == 'A'
        assert lp1b == lp1
        self.commit_transaction()
        #
        self.switch(0)
        assert h[9991234] == lp2
        assert stm_get_char(lp2) == 'B'
        assert h[1234] == lp1
        h[1234] = ffi.NULL
        self.commit_transaction()
        h.free()

    def test_conflict(self):
        h = StmHashTable()
        lp1 = stm_allocate_old(16)
        lp2 = stm_allocate_old(16)
        #
        self.start_transaction()
        h[1234] = lp1
        #
        self.switch(1)
        self.start_transaction()
        py.test.raises(Conflict, "h[1234] = lp2")
