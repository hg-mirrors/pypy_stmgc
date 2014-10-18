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
