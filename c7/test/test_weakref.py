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





# class TestMajorCollection(BaseTest):

#     def test_weakref_old(self):
#         p2 = nalloc(HDR)
#         p1 = lib.stm_weakref_allocate(WEAKREF_SIZE, WEAKREF_TID, p2)
#         #
#         lib.stm_push_root(p1)
#         lib.stm_push_root(p2)
#         major_collect()
#         p2 = lib.stm_pop_root()
#         p1 = lib.stm_pop_root()
#         assert lib.rawgetptr(p1, 0) == p2
#         #
#         lib.stm_push_root(p1)
#         major_collect()
#         p1 = lib.stm_pop_root()
#         assert lib.rawgetptr(p1, 0) == ffi.NULL

#     def test_weakref_to_prebuilt(self):
#         p2 = palloc(HDR)
#         p1 = lib.stm_weakref_allocate(WEAKREF_SIZE, WEAKREF_TID, p2)
#         #
#         lib.stm_push_root(p1)
#         major_collect()
#         p1 = lib.stm_pop_root()
#         assert lib.rawgetptr(p1, 0) == p2

#     def test_weakref_update_version(self):
#         p2 = oalloc(HDR + WORD); make_public(p2)
#         p1 = lib.stm_weakref_allocate(WEAKREF_SIZE, WEAKREF_TID, p2)
#         #
#         lib.stm_push_root(p1)
#         lib.stm_push_root(p2)
#         major_collect()
#         p2 = lib.stm_pop_root()
#         p1 = lib.stm_pop_root()
#         assert lib.rawgetptr(p1, 0) == p2
#         #
#         lib.stm_commit_transaction()
#         lib.stm_begin_inevitable_transaction()
#         #
#         lib.setlong(p2, 0, 912809218)   # write barrier
#         assert lib.rawgetlong(p2, 0) == 0
#         lib.stm_push_root(p1)
#         lib.stm_push_root(p2)
#         major_collect()
#         p2 = lib.stm_pop_root()
#         p1 = lib.stm_pop_root()
#         assert lib.rawgetptr(p1, 0) == p2
#         assert lib.rawgetlong(p2, 0) == 0
#         #
#         lib.stm_commit_transaction()
#         lib.stm_begin_inevitable_transaction()
#         #
#         assert lib.rawgetptr(p1, 0) == p2
#         assert lib.rawgetlong(p2, 0) == 0
#         lib.stm_push_root(p1)
#         lib.stm_push_root(p2)
#         major_collect()
#         p2b = lib.stm_pop_root()
#         p1 = lib.stm_pop_root()
#         assert lib.rawgetptr(p1, 0) == p2
#         assert p2b != p2
#         assert lib.getlong(p2b, 0) == 912809218
#         assert lib.getlong(p2, 0) == 912809218


#     def test_stealing(self):
#         p = palloc_refs(1)
#         u = palloc_refs(1)

#         def f1(r):
#             q = nalloc(HDR+WORD)
#             # lib.stm_push_root(q)
#             w = lib.stm_weakref_allocate(WEAKREF_SIZE, WEAKREF_TID, q)
#             # q = lib.stm_pop_root()
#             setptr(p, 0, w)
#             setptr(u, 0, q)
#             minor_collect()
#             lib.stm_commit_transaction()
#             lib.stm_begin_inevitable_transaction()
#             r.set(2)
#             r.wait(3)
#             print "happy"

#         def f2(r):
#             r.wait(2)
#             # steal p, should stub the weakref contained in it
#             pr = lib.stm_read_barrier(p)
#             w = rawgetptr(pr, 0)
#             assert classify(w) == "stub"

#             # read weakref, should stub out weakptr
#             wr = lib.stm_read_barrier(w)
#             assert wr.h_tid & GCFLAG_WEAKREF
#             assert classify(lib.rawgetptr(wr, 0)) == "stub"

#             r.set(3)

#         run_parallel(f1, f2)
