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

    def test_creation_marker_in_nursery(self):
        self.start_transaction()
        lp1 = stm_allocate(16)
        lp2 = stm_allocate(16)
        assert stm_creation_marker(lp1) == 0xff
        assert stm_creation_marker(lp2) == 0xff
        u1 = int(ffi.cast("uintptr_t", lp1))
        u2 = int(ffi.cast("uintptr_t", lp2))
        assert u2 == u1 + 16
        self.commit_transaction()

        assert stm_creation_marker(lp1) == 0
        assert stm_creation_marker(lp2) == 0

        self.start_transaction()
        lp3 = stm_allocate(16)
        assert stm_creation_marker(lp1) == 0
        assert stm_creation_marker(lp2) == 0
        assert stm_creation_marker(lp3) == 0xff

    def test_nursery_medium(self):
        self.start_transaction()
        lp1 = stm_allocate(SOME_MEDIUM_SIZE)
        lp2 = stm_allocate(SOME_MEDIUM_SIZE)

        u1 = int(ffi.cast("uintptr_t", lp1))
        u2 = int(ffi.cast("uintptr_t", lp2))
        assert (u1 & 255) == 0
        assert (u2 & 255) == 0
        assert stm_creation_marker(lp1) == 0xff
        assert stm_creation_marker(lp2) == 0xff

        self.commit_transaction()
        assert stm_creation_marker(lp1) == 0
        assert stm_creation_marker(lp2) == 0
