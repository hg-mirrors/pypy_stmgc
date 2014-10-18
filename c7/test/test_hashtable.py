from support import *
import random
import py

class TestHashtable(BaseTest):

    def test_empty(self):
        self.start_transaction()
        h = lib.stm_hashtable_create()
        for i in range(100):
            index = random.randrange(0, 1<<64)
            got = lib.stm_hashtable_read(h, index)
            assert got == ffi.NULL
        lib.stm_hashtable_free(h)

    def test_set_value(self):
        self.start_transaction()
        h = lib.stm_hashtable_create()
        lp1 = stm_allocate(16)
        lib.stm_hashtable_write(h, 12345678901, lp1)
        assert lib.stm_hashtable_read(h, 12345678901) == lp1
        for i in range(64):
            index = 12345678901 ^ (1 << i)
            assert lib.stm_hashtable_read(h, index) == ffi.NULL
        assert lib.stm_hashtable_read(h, 12345678901) == lp1
        lib.stm_hashtable_free(h)
