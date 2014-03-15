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


    def test_multiple_threads(self):
        self.start_transaction()
        lp0 = stm_allocate(1024)
        self.push_root(lp0)
        self.commit_transaction()

        self.start_transaction()
        lp0 = self.pop_root()
        self.push_root(lp0)
        stm_write(lp0) # privatize page

        self.push_root_no_gc()
        lp2 = stm_allocate(48)
        lp1 = stm_allocate_weakref(lp2)    # no collection here
        self.pop_root()

        self.push_root(lp0)
        self.push_root(lp1)
        self.commit_transaction()
        # lp2 dies
        lp1 = self.pop_root()
        self.push_root(lp1)

        assert stm_get_weakref(lp1) == ffi.NULL

        self.switch(1)

        self.start_transaction()
        assert stm_get_weakref(lp1) == ffi.NULL


class TestIsolation(BaseTest):
    def test_not_break(self):
        py.test.xfail("known to fail")
        lpold = stm_allocate_old_refs(1)

        self.start_transaction()
        toref = stm_allocate(16)
        weakref = stm_allocate_weakref(toref)
        stm_set_ref(lpold, 0, weakref)
        self.push_root(toref)
        self.commit_transaction()
        # toref survives because it's on our shadowstack
        # weakref is in a prebuilt old object

        # get the new toref reference (old now)
        toref = self.pop_root()
        self.push_root(toref)

        self.switch(1)
        # in another thread, we start a transaction

        self.start_transaction()
        weakref = stm_get_ref(lpold, 0)
        # everyone is still alive, so we still have toref in the weakref:
        assert stm_get_weakref(weakref) == toref


        self.switch(0)

        # back in thread 0, we pop toref from the shadowstack
        # in an inevitable transaction
        self.start_transaction()
        stm_become_inevitable()
        self.pop_root() # forget toref
        stm_major_collect()

        self.switch(1)

        # toref should still be alive and stored in the
        # weakref...
        # should not be NULLed, otherwise this means we can
        # communicate between the inevitable and this transaction!
        assert stm_get_weakref(weakref) == toref


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

    def test_multiple_threads(self):
        self.start_transaction()
        lp0 = stm_allocate(48)
        lp1 = stm_allocate_weakref(lp0)    # no collection here
        self.push_root(lp1)
        self.push_root(lp0)
        self.commit_transaction()

        self.start_transaction()
        lp0 = self.pop_root()
        lp1 = self.pop_root()
        self.push_root(lp1)

        stm_write(lp0) # privatize page with weakref in it too

        assert stm_get_page_flag(stm_get_obj_pages(lp1)[0]) == PRIVATE_PAGE
        assert stm_get_weakref(lp1) == lp0

        self.commit_transaction()
        self.start_transaction()

        # lp0 dies
        stm_major_collect()

        assert stm_get_weakref(lp1) == ffi.NULL
        print stm_get_real_address(lp1)

        self.switch(1)

        self.start_transaction()
        assert stm_get_weakref(lp1) == ffi.NULL
        print stm_get_real_address(lp1)

    def test_prebuit_weakref(self):
        from test_prebuilt import prebuilt
        static1 = prebuilt(16)     # a prebuit dead weakref
        lp1 = lib.stm_setup_prebuilt_weakref(static1)
        static2 = prebuilt(16)     # some random prebuilt object
        ffi.cast("char *", static2)[8:11] = 'ABC'
        lp2 = lib.stm_setup_prebuilt(static2)
        static3 = prebuilt(16)     # a prebuilt weakref to static2
        ffi.cast("object_t **", static3)[1] = static2
        lp3 = lib.stm_setup_prebuilt_weakref(static3)
        #
        self.start_transaction()
        assert stm_get_char(lp2, 8) == 'A'
        assert stm_get_char(lp2, 9) == 'B'
        assert stm_get_char(lp2, 10) == 'C'
        assert stm_get_weakref(lp1) == ffi.NULL
        assert stm_get_weakref(lp3) == lp2
