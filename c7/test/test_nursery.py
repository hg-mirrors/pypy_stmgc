from support import *
import py

class TestBasic(BaseTest):

    def test_align_nursery_to_256_bytes(self):
        self.start_transaction()
        lp1 = stm_allocate(16)
        self.commit_transaction()
        self.start_transaction()
        lp2 = stm_allocate(16)
        #
        u1 = int(ffi.cast("uintptr_t", lp1))
        u2 = int(ffi.cast("uintptr_t", lp2))
        assert (u1 & ~255) != (u2 & ~255)
