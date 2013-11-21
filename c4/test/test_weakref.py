import py
from support import *


class BaseTest(object):
    def setup_method(self, meth):
        lib.stm_clear_between_tests()
        lib.stm_initialize_tests(0)
    def teardown_method(self, meth):
        lib.stm_finalize()


class TestMinorCollection(BaseTest):

    def test_weakref_invalidate(self):
        p2 = nalloc(HDR)
        p1 = lib.stm_weakref_allocate(WEAKREF_SIZE, WEAKREF_TID, p2)
        assert p1.h_tid == WEAKREF_TID | GCFLAG_IMMUTABLE | GCFLAG_WEAKREF
        assert p1.h_revision == lib.get_private_rev_num()
        assert lib.rawgetptr(p1, 0) == p2
        lib.stm_push_root(p1)
        minor_collect()
        p1 = lib.stm_pop_root()
        assert lib.rawgetptr(p1, 0) == ffi.NULL

    def test_weakref_itself_dies(self):
        p2 = nalloc(HDR)
        p1 = lib.stm_weakref_allocate(WEAKREF_SIZE, WEAKREF_TID, p2)
        minor_collect()

    def test_weakref_keep(self):
        p2 = nalloc(HDR)
        p1 = lib.stm_weakref_allocate(WEAKREF_SIZE, WEAKREF_TID, p2)
        assert p1.h_tid == WEAKREF_TID | GCFLAG_IMMUTABLE | GCFLAG_WEAKREF
        assert p1.h_revision == lib.get_private_rev_num()
        assert lib.rawgetptr(p1, 0) == p2
        lib.stm_push_root(p1)
        lib.stm_push_root(p2)
        minor_collect()
        p2 = lib.stm_pop_root()
        p1 = lib.stm_pop_root()
        assert lib.rawgetptr(p1, 0) == p2

    def test_weakref_old_keep(self):
        p2 = oalloc(HDR)
        p1 = lib.stm_weakref_allocate(WEAKREF_SIZE, WEAKREF_TID, p2)
        assert p1.h_tid == WEAKREF_TID | GCFLAG_IMMUTABLE | GCFLAG_WEAKREF
        assert p1.h_revision == lib.get_private_rev_num()
        assert lib.rawgetptr(p1, 0) == p2
        lib.stm_push_root(p1)
        lib.stm_push_root(p2)
        minor_collect()
        p2 = lib.stm_pop_root()
        p1 = lib.stm_pop_root()
        assert lib.rawgetptr(p1, 0) == p2

        
    def test_old_private_not_keep_alive_weakref(self):
        p = palloc(HDR + WORD)
        q = palloc_refs(1)

        def f1(c):
            if c == 1:
                # currently fails because:
                # p1 still in old_objects_to_trace
                # -> keeps alive weakp1w
                # -> stm_move_young_weakrefs() sees a weakref pointing
                #    to an aborted object
                minor_collect()
                return

            # allocate the "container" as old, private q1
            q1 = lib.stm_write_barrier(q)
            assert classify(q1) == "private"
            lib.stm_push_root(q1)
            minor_collect()
            q1 = lib.stm_pop_root()
            assert classify(q1) == "private"
            assert q1.h_tid & GCFLAG_OLD
            assert q1.h_tid & GCFLAG_WRITE_BARRIER

            # allocate young private p1 to point to
            p1 = lib.stm_write_barrier(p)
            assert ffi.cast("gcptr", p1.h_original) == p
            assert classify(p1) == "private"
            assert not (p1.h_tid & GCFLAG_OLD)

            lib.stm_push_root(p1)
            lib.stm_push_root(q1)
            weakp1w = lib.stm_weakref_allocate(WEAKREF_SIZE, WEAKREF_TID, p1)
            q1 = lib.stm_pop_root()
            p1 = lib.stm_pop_root()
            # q1 still old, p1 still young, weakp1w also young

            q1w = lib.stm_write_barrier(q1)
            # add q1 to old_objects_to_trace
            assert q1 == q1w # was and is private
            lib.rawsetptr(q1, 0, weakp1w)

            abort_and_retry()

        perform_transaction(f1)            


        



        



class TestMajorCollection(BaseTest):

    def test_weakref_old(self):
        p2 = nalloc(HDR)
        p1 = lib.stm_weakref_allocate(WEAKREF_SIZE, WEAKREF_TID, p2)
        #
        lib.stm_push_root(p1)
        lib.stm_push_root(p2)
        major_collect()
        p2 = lib.stm_pop_root()
        p1 = lib.stm_pop_root()
        assert lib.rawgetptr(p1, 0) == p2
        #
        lib.stm_push_root(p1)
        major_collect()
        p1 = lib.stm_pop_root()
        assert lib.rawgetptr(p1, 0) == ffi.NULL

    def test_weakref_to_prebuilt(self):
        p2 = palloc(HDR)
        p1 = lib.stm_weakref_allocate(WEAKREF_SIZE, WEAKREF_TID, p2)
        #
        lib.stm_push_root(p1)
        major_collect()
        p1 = lib.stm_pop_root()
        assert lib.rawgetptr(p1, 0) == p2

    def test_weakref_update_version(self):
        p2 = oalloc(HDR + WORD); make_public(p2)
        p1 = lib.stm_weakref_allocate(WEAKREF_SIZE, WEAKREF_TID, p2)
        #
        lib.stm_push_root(p1)
        lib.stm_push_root(p2)
        major_collect()
        p2 = lib.stm_pop_root()
        p1 = lib.stm_pop_root()
        assert lib.rawgetptr(p1, 0) == p2
        #
        lib.stm_commit_transaction()
        lib.stm_begin_inevitable_transaction()
        #
        lib.setlong(p2, 0, 912809218)   # write barrier
        assert lib.rawgetlong(p2, 0) == 0
        lib.stm_push_root(p1)
        lib.stm_push_root(p2)
        major_collect()
        p2 = lib.stm_pop_root()
        p1 = lib.stm_pop_root()
        assert lib.rawgetptr(p1, 0) == p2
        assert lib.rawgetlong(p2, 0) == 0
        #
        lib.stm_commit_transaction()
        lib.stm_begin_inevitable_transaction()
        #
        assert lib.rawgetptr(p1, 0) == p2
        assert lib.rawgetlong(p2, 0) == 0
        lib.stm_push_root(p1)
        lib.stm_push_root(p2)
        major_collect()
        p2b = lib.stm_pop_root()
        p1 = lib.stm_pop_root()
        assert lib.rawgetptr(p1, 0) == p2
        assert p2b != p2
        assert lib.getlong(p2b, 0) == 912809218
        assert lib.getlong(p2, 0) == 912809218


    def test_stealing(self):
        p = palloc_refs(1)
        u = palloc_refs(1)
        
        def f1(r):
            q = nalloc(HDR+WORD)
            # lib.stm_push_root(q)
            w = lib.stm_weakref_allocate(WEAKREF_SIZE, WEAKREF_TID, q)
            # q = lib.stm_pop_root()
            setptr(p, 0, w)
            setptr(u, 0, q)
            minor_collect()
            lib.stm_commit_transaction()
            lib.stm_begin_inevitable_transaction()
            r.set(2)
            r.wait(3)
            print "happy"
            
        def f2(r):
            r.wait(2)
            # steal p, should stub the weakref contained in it
            pr = lib.stm_read_barrier(p)
            w = rawgetptr(pr, 0)
            assert classify(w) == "stub"

            # read weakref, should stub out weakptr
            wr = lib.stm_read_barrier(w)
            assert wr.h_tid & GCFLAG_WEAKREF
            assert classify(lib.rawgetptr(wr, 0)) == "stub"

            r.set(3)
            
        run_parallel(f1, f2)


