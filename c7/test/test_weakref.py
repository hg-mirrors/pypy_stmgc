import py
from support import *




class TestMinorCollection(BaseTest):
    def test_simple(self):
        lib._stm_set_nursery_free_count(2048)
        self.start_transaction()

        self.push_root_no_gc()
        lp2 = stm_allocate(48)
        lp1 = stm_allocate_weakref(lp2)    # no collection here
        self.pop_root()

        assert stm_get_weakref(lp1) == lp2

        self.push_root(lp1)
        stm_minor_collect()
        lp1 = self.pop_root()
        # lp2 died
        assert stm_get_weakref(lp1) == ffi.NULL

        self.push_root(lp1)
        stm_minor_collect()
        lp1 = self.pop_root()
        # lp2 died
        assert stm_get_weakref(lp1) == ffi.NULL

    def test_still_simple(self):
        lib._stm_set_nursery_free_count(2048)
        self.start_transaction()

        self.push_root_no_gc()
        lp2 = stm_allocate(48)
        lp1 = stm_allocate_weakref(lp2)    # no collection here
        self.pop_root()

        assert stm_get_weakref(lp1) == lp2

        self.push_root(lp1)
        self.push_root(lp2)
        stm_minor_collect()
        lp2 = self.pop_root()
        lp1 = self.pop_root()
        # lp2 survived
        assert stm_get_weakref(lp1) == lp2

        self.push_root(lp1)
        self.push_root(lp2)
        stm_minor_collect()
        lp2 = self.pop_root()
        lp1 = self.pop_root()
        # lp2 survived
        assert stm_get_weakref(lp1) == lp2

    def test_weakref_itself_dies(self):
        self.start_transaction()

        self.push_root_no_gc()
        lp2 = stm_allocate(48)
        stm_allocate_weakref(lp2)    # no collection here
        self.pop_root()
        stm_minor_collect()
        assert lib._stm_total_allocated() == 0


    def test_weakref_old_keep(self):
        lp0 = stm_allocate_old(48)

        self.start_transaction()
        self.push_root_no_gc()
        lp1 = stm_allocate_weakref(lp0)    # no collection here
        self.pop_root()

        self.push_root(lp1)
        stm_minor_collect()
        lp1 = self.pop_root()

        assert stm_get_weakref(lp1) == lp0


    def test_abort_cleanup(self):
        self.start_transaction()

        self.push_root_no_gc()
        lp1 = stm_allocate_weakref(ffi.NULL)    # no collection here
        self.pop_root()

        self.abort_transaction()
        self.start_transaction()

    def test_big_alloc_sizes(self):
        sizes = [lib._STM_FAST_ALLOC + 16, 48,]

        for osize in sizes:
            self.start_transaction()
            self.push_root_no_gc()
            lp2 = stm_allocate(osize)
            lp1 = stm_allocate_weakref(lp2)    # no collection here
            self.pop_root()

            assert stm_get_weakref(lp1) == lp2

            self.push_root(lp1)
            self.push_root(lp2)
            stm_minor_collect()
            lp2 = self.pop_root()
            lp1 = self.pop_root()
            # lp2 survived
            assert stm_get_weakref(lp1) == lp2
            self.abort_transaction()



class TestMajorCollection(BaseTest):
    def test_simple(self):
        self.start_transaction()

        self.push_root_no_gc()
        lp2 = stm_allocate(48)
        lp1 = stm_allocate_weakref(lp2)    # no collection here
        self.pop_root()

        assert stm_get_weakref(lp1) == lp2

        self.push_root(lp1)
        self.push_root(lp2)
        stm_minor_collect()
        lp2 = self.pop_root()
        lp1 = self.pop_root()
        # lp2 survived
        assert stm_get_weakref(lp1) == lp2

        self.push_root(lp1)
        stm_minor_collect()
        lp1 = self.pop_root()
        # lp2 survived because no major collection
        assert stm_get_weakref(lp1) == lp2

        self.push_root(lp1)
        stm_major_collect()
        lp1 = self.pop_root()
        # lp2 died
        assert stm_get_weakref(lp1) == ffi.NULL

    def test_weakref_old_keep(self):
        lp0 = stm_allocate_old(48)

        self.start_transaction()
        self.push_root_no_gc()
        lp1 = stm_allocate_weakref(lp0)    # no collection here
        self.pop_root()

        self.push_root(lp1)
        stm_major_collect()
        lp1 = self.pop_root()

        assert stm_get_weakref(lp1) == lp0

    def test_survive(self):
        self.start_transaction()

        self.push_root_no_gc()
        lp2 = stm_allocate(48)
        lp1 = stm_allocate_weakref(lp2)    # no collection here
        self.pop_root()

        assert stm_get_weakref(lp1) == lp2

        self.push_root(lp1)
        self.push_root(lp2)
        stm_major_collect()
        lp2 = self.pop_root()
        lp1 = self.pop_root()
        # lp2 survived
        assert stm_get_weakref(lp1) == lp2

        self.push_root(lp1)
        stm_minor_collect()
        lp1 = self.pop_root()
        # lp2 survived because no major collection
        assert stm_get_weakref(lp1) == lp2

        self.push_root(lp1)
        stm_major_collect()
        lp1 = self.pop_root()
        # lp2 died
        assert stm_get_weakref(lp1) == ffi.NULL
