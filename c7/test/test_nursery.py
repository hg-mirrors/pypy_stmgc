from support import *
import py

class TestBasic(BaseTest):

    def test_nursery_full(self):
        lib._stm_set_nursery_free_count(2048)
        self.start_transaction()
        self.push_root_no_gc()
        lp1 = stm_allocate(2048)    # no collection here
        self.pop_root()
        #
        self.push_root(lp1)
        lp2 = stm_allocate(2048)
        lp1b = self.pop_root()
        assert lp1b != lp1      # collection occurred

    def test_several_minor_collections(self):
        # make a long, ever-growing linked list of objects, in one transaction
        lib._stm_set_nursery_free_count(2048)
        self.start_transaction()
        lp1 = stm_allocate_refs(1)
        self.push_root(lp1)
        prev = lp1
        prevprev = None
        FIT = 2048 / 16 - 1   # without 'lp1' above
        N = 4096 / 16 + 41
        for i in range(N):
            if prevprev:
                assert stm_get_ref(prevprev, 0) == prev
                self.push_root(prevprev)
            self.push_root(prev)
            lp3 = stm_allocate_refs(1)
            prev = self.pop_root()
            if prevprev:
                prevprev = self.pop_root()
                assert prevprev != prev
            stm_set_ref(prev, 0, lp3)

            assert modified_old_objects() == []    # only 1 transaction
            opn = objects_pointing_to_nursery()
            if i < FIT:
                assert opn is None      # no minor collection so far
            else:
                assert len(opn) == 1

            prevprev = prev
            prev = lp3

        lp1 = self.pop_root()
        assert modified_old_objects() == []

        lp2 = lp1
        for i in range(N):
            assert lp2
            prev = lp2
            lp2 = stm_get_ref(lp2, 0)
        assert lp2 == lp3

    def test_many_allocs(self):
        lib._stm_set_nursery_free_count(32768)
        obj_size = 512
        num = 65536 / obj_size + 41

        self.start_transaction()
        for i in range(num):
            new = stm_allocate(obj_size)
            stm_set_char(new, chr(i % 255))
            self.push_root(new)

        old = []
        young = []
        for i in reversed(range(num)):
            r = self.pop_root()
            assert stm_get_char(r) == chr(i % 255)
            if is_in_nursery(r):
                young.append(r)
            else:
                old.append(r)

        assert old
        assert young

    def test_larger_than_limit_for_nursery_die(self):
        obj_size = lib._STM_FAST_ALLOC + 16

        self.start_transaction()
        assert lib._stm_total_allocated() == 0
        seen = set()
        for i in range(10):
            stm_minor_collect()
            new = stm_allocate(obj_size)
            assert not is_in_nursery(new)
            assert lib._stm_total_allocated() == obj_size + 16
            seen.add(new)
        assert len(seen) < 5     # addresses are reused
        stm_minor_collect()
        assert lib._stm_total_allocated() == 0

    def test_larger_than_limit_for_nursery_dont_die(self):
        obj_nrefs = (lib._STM_FAST_ALLOC + 16) // 8

        self.start_transaction()
        lp1 = ffi.cast("object_t *", 0)
        seen = set()
        for i in range(100):
            self.push_root(lp1)
            stm_minor_collect()
            lp1 = self.pop_root()
            new = stm_allocate_refs(obj_nrefs)
            assert not is_in_nursery(new)
            seen.add(new)
            stm_set_ref(new, i, lp1)
            lp1 = new
        assert len(seen) == 100     # addresses are not reused

        for i in reversed(range(100)):
            assert lp1
            lp1 = stm_get_ref(lp1, i)
        assert not lp1

    def test_account_for_privatized_page(self):
        self.start_transaction()
        obj = stm_allocate(16)
        self.push_root(obj)
        self.commit_transaction()
        obj = self.pop_root()
        base = lib._stm_total_allocated()
        assert base <= 4096

        self.start_transaction()
        stm_write(obj)
        assert lib._stm_total_allocated() == base + 4096

    def test_reset_partial_alloc_pages(self):
        py.test.skip("a would-be-nice feature, but not actually needed: "
                     "the next major GC will take care of it")
        self.start_transaction()
        new = stm_allocate(16)
        stm_set_char(new, 'a')
        self.push_root(new)
        stm_minor_collect()
        new = self.pop_root()
        self.abort_transaction()

        self.start_transaction()
        newer = stm_allocate(16)
        self.push_root(newer)
        stm_minor_collect()
        newer = self.pop_root()
        assert stm_get_real_address(new) == stm_get_real_address(newer)
        assert stm_get_char(newer) == '\0'

    def test_reuse_page(self):
        py.test.skip("a would-be-nice feature, but not actually needed: "
                     "the next major GC will take care of it")
        self.start_transaction()
        new = stm_allocate(16)
        self.push_root(new)
        stm_minor_collect()
        new = self.pop_root()
        # assert stm_get_page_flag(stm_get_obj_pages(new)[0]) == lib.UNCOMMITTED_SHARED_PAGE
        self.abort_transaction()

        self.start_transaction()
        newer = stm_allocate(16)
        self.push_root(newer)
        stm_minor_collect()
        newer = self.pop_root()
        assert new == newer

    def test_write_to_old_after_minor(self):
        self.start_transaction()
        new = stm_allocate(16)
        self.push_root(new)
        stm_minor_collect()
        old = self.pop_root()
        self.commit_transaction()

        self.start_transaction()
        stm_write(old) # old objs to trace
        stm_set_char(old, 'x')
        stm_minor_collect()
        stm_write(old) # old objs to trace
        stm_set_char(old, 'y')
        self.commit_transaction()
