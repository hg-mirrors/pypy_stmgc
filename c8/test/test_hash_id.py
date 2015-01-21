from support import *
from test_prebuilt import prebuilt
import py

class TestHashId(BaseTest):

    def test_hash_old_object(self):
        lp1 = stm_allocate_old(16)
        lp2 = stm_allocate_old(16)
        lp3 = stm_allocate_old(16)
        lp4 = stm_allocate_old(16)
        self.start_transaction()
        h1 = lib.stm_identityhash(lp1)
        h2 = lib.stm_identityhash(lp2)
        h3 = lib.stm_identityhash(lp3)
        h4 = lib.stm_identityhash(lp4)
        assert len(set([h1, h2, h3, h4])) == 4     # guaranteed by the algo

    def test_id_old_object(self):
        lp1 = stm_allocate_old(16)
        self.start_transaction()
        h1 = lib.stm_id(lp1)
        assert h1 == int(ffi.cast("long", lp1))

    def test_set_prebuilt_identityhash(self):
        static1 = prebuilt(16)
        static2 = prebuilt(16)
        lp1 = lib.stm_setup_prebuilt(static1)
        lp2 = lib.stm_setup_prebuilt(static2)
        lib.stm_set_prebuilt_identityhash(lp1, 42)
        self.start_transaction()
        h1 = lib.stm_identityhash(lp1)
        h2 = lib.stm_identityhash(lp2)
        assert h1 == 42
        assert h2 != 0
        h1 = lib.stm_id(lp1)
        h2 = lib.stm_id(lp2)
        assert h1 == int(ffi.cast("long", lp1))
        assert h2 == int(ffi.cast("long", lp2))

    def test_hash_nursery(self):
        self.start_transaction()
        lp1 = stm_allocate(16)
        lp2 = stm_allocate(16)
        lp3 = stm_allocate(16)
        lp4 = stm_allocate(16)
        h1 = lib.stm_identityhash(lp1)
        h2 = lib.stm_identityhash(lp2)
        h3 = lib.stm_identityhash(lp3)
        h4 = lib.stm_identityhash(lp4)
        assert len(set([h1, h2, h3, h4])) == 4     # guaranteed by the algo

    def test_hash_lower_bits(self):
        py.test.skip("fails without a real large-malloc implementation")
        self.start_transaction()
        lp1 = stm_allocate(32)
        lp2 = stm_allocate(32)
        lp3 = stm_allocate(32)
        lp4 = stm_allocate(32)
        h1 = lib.stm_identityhash(lp1)
        h2 = lib.stm_identityhash(lp2)
        h3 = lib.stm_identityhash(lp3)
        h4 = lib.stm_identityhash(lp4)
        assert len(set([h1 & 15, h2 & 15, h3 & 15, h4 & 15])) == 4

    def test_hash_around_minor_collect(self):
        self.start_transaction()
        lp = stm_allocate(16)
        h1 = lib.stm_identityhash(lp)
        self.push_root(lp)
        stm_minor_collect()
        lp = self.pop_root()
        h2 = lib.stm_identityhash(lp)
        assert h2 == h1
        assert h2 != lib.stm_id(lp)
