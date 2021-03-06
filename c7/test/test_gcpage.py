from support import *
import py


LMO = LARGE_MALLOC_OVERHEAD


class TestGCPage(BaseTest):

    def test_large_obj_alloc(self):
        # test obj which doesn't fit into the size_classes
        # for now, we will still allocate it in the nursery.
        # expects: GC_N_SMALL_REQUESTS  36
        size_class = 1000 # too big
        obj_size = size_class * 8
        assert obj_size > 4096 # we want more than 1 page
        assert obj_size < lib._STM_FAST_ALLOC # in the nursery

        self.start_transaction()
        new = stm_allocate(obj_size)
        assert is_in_nursery(new)
        self.push_root(new)
        stm_minor_collect()
        new = self.pop_root()

        assert len(stm_get_obj_pages(new)) == 2
        assert ([stm_get_private_page(p) for p in stm_get_obj_pages(new)]
                == [0, 0])

        assert not is_in_nursery(new)
        stm_write(new)
        self.commit_transaction()

        # now proceed to write into the object in a new transaction
        self.start_transaction()
        assert ([stm_get_private_page(p) for p in stm_get_obj_pages(new)]
                == [0, 0])
        stm_write(new)
        assert ([bool(stm_get_private_page(p)) for p in stm_get_obj_pages(new)]
                == [True, True])

        # write to 2nd page of object!!
        wnew = stm_get_real_address(new)
        wnew[4097] = 'x'

        self.switch(1)
        self.start_transaction()
        stm_read(new)
        rnew = stm_get_real_address(new)
        assert rnew[4097] == '\0'
        self.abort_transaction()

        self.switch(0)
        self.abort_transaction()
        assert ([bool(stm_get_private_page(p)) for p in stm_get_obj_pages(new)]
                == [True, True])

    def test_partial_alloced_pages(self):
        self.start_transaction()
        new = stm_allocate(16)
        self.push_root(new)
        stm_minor_collect()
        new = self.pop_root()

        assert stm_get_private_page(stm_get_obj_pages(new)[0]) == 0
        assert stm_get_flags(new) & GCFLAG_WRITE_BARRIER

        stm_write(new)
        assert not (stm_get_flags(new) & GCFLAG_WRITE_BARRIER)

        self.commit_transaction()
        assert stm_get_private_page(stm_get_obj_pages(new)[0]) == 0
        assert stm_get_flags(new) & GCFLAG_WRITE_BARRIER

        self.start_transaction()
        newer = stm_allocate(16)
        self.push_root(newer)
        stm_minor_collect()
        newer = self.pop_root()
        # 'new' is still in shared_page and committed
        assert stm_get_private_page(stm_get_obj_pages(new)[0]) == 0
        assert stm_get_flags(new) & GCFLAG_WRITE_BARRIER
        # 'newer' is now part of the SHARED page with 'new', but
        # uncommitted, so no privatization has to take place:
        assert stm_get_obj_pages(new) == stm_get_obj_pages(newer)
        assert stm_get_flags(newer) & GCFLAG_WRITE_BARRIER
        stm_write(newer) # does not privatize
        assert not (stm_get_flags(newer) & GCFLAG_WRITE_BARRIER)
        assert stm_get_private_page(stm_get_obj_pages(newer)[0]) == 0
        self.commit_transaction()

        assert stm_get_private_page(stm_get_obj_pages(newer)[0]) == 0
        assert stm_get_flags(newer) & GCFLAG_WRITE_BARRIER

    def test_major_collection(self):
        self.start_transaction()
        new = stm_allocate(5000)
        self.push_root(new)
        stm_minor_collect()
        assert lib._stm_total_allocated() == 5000 + LMO

        self.pop_root()
        stm_minor_collect()
        assert lib._stm_total_allocated() == 5000 + LMO

        stm_major_collect()
        assert lib._stm_total_allocated() == 0

    def test_mark_recursive(self):
        def make_chain(sz):
            prev = ffi.cast("object_t *", ffi.NULL)
            for i in range(10):
                self.push_root(prev)
                new = stm_allocate_refs(sz/8-1)
                prev = self.pop_root()
                stm_set_ref(new, 42, prev)
                prev = new
            return prev

        self.start_transaction()
        self.push_root(make_chain(5000))
        self.push_root(make_chain(4312))
        stm_minor_collect()
        assert lib._stm_total_allocated() == (10 * (5000 + LMO) +
                                              10 * (4312 + LMO))
        stm_major_collect()
        assert lib._stm_total_allocated() == (10 * (5000 + LMO) +
                                              10 * (4312 + LMO))
        stm_major_collect()
        assert lib._stm_total_allocated() == (10 * (5000 + LMO) +
                                              10 * (4312 + LMO))
        self.pop_root()
        stm_major_collect()
        assert lib._stm_total_allocated() == 10 * (5000 + LMO)

    def test_trace_all_versions(self):
        self.start_transaction()
        x = stm_allocate(5000)
        stm_set_char(x, 'A')
        stm_set_char(x, 'a', 4999)
        self.push_root(x)
        self.commit_transaction()
        assert lib._stm_total_allocated() == 5000 + LMO

        self.start_transaction()
        x = self.pop_root()
        self.push_root(x)
        assert lib._stm_total_allocated() == 5000 + LMO
        stm_set_char(x, 'B')
        stm_set_char(x, 'b', 4999)
        assert lib._stm_total_allocated() == 5000 + LMO + 2 * 4096  # 2 pages
        stm_major_collect()

        assert stm_get_char(x)       == 'B'
        assert stm_get_char(x, 4999) == 'b'

        self.switch(1)
        self.start_transaction()
        assert stm_get_char(x)       == 'A'
        assert stm_get_char(x, 4999) == 'a'

        self.switch(0)
        assert stm_get_char(x)       == 'B'
        assert stm_get_char(x, 4999) == 'b'
        assert lib._stm_total_allocated() == 5000 + LMO + 2 * 4096  # 2 pages

    def test_trace_correct_version_of_overflow_objects_1(self, size=32):
        self.start_transaction()
        #
        self.switch(1)
        self.start_transaction()
        x = stm_allocate(size)
        stm_set_char(x, 'E', size - 1)
        self.push_root(x)
        #
        self.switch(0)
        stm_major_collect()
        #
        self.switch(1)
        x = self.pop_root()
        assert stm_get_char(x, size - 1) == 'E'

    def test_trace_correct_version_of_overflow_objects_2(self):
        self.test_trace_correct_version_of_overflow_objects_1(size=5000)

    def test_reshare_if_no_longer_modified_0(self, invert=0):
        if invert:
            self.switch(1)
        self.start_transaction()
        x = stm_allocate(5000)
        self.push_root(x)
        self.commit_transaction()
        x = self.pop_root()
        #
        self.switch(1 - invert)
        self.start_transaction()
        self.push_root(x)
        stm_set_char(x, 'A')
        stm_major_collect()
        assert lib._stm_total_allocated() == 5000 + LMO + 2 * 4096  # 2 pages
        self.commit_transaction()
        #
        self.start_transaction()
        stm_major_collect()
        assert lib._stm_total_allocated() == 5000 + LMO    # shared again

    def test_reshare_if_no_longer_modified_1(self):
        self.test_reshare_if_no_longer_modified_0(invert=1)

    def test_threadlocal_at_start_of_transaction(self):
        self.start_transaction()
        x = stm_allocate(16)
        stm_set_char(x, 'L')
        self.set_thread_local_obj(x)
        self.commit_transaction()

        self.start_transaction()
        assert stm_get_char(self.get_thread_local_obj()) == 'L'
        self.set_thread_local_obj(stm_allocate(32))
        stm_minor_collect()
        self.abort_transaction()

        self.start_transaction()
        assert stm_get_char(self.get_thread_local_obj()) == 'L'
        self.set_thread_local_obj(stm_allocate(32))
        stm_major_collect()
        self.abort_transaction()

        self.start_transaction()
        assert stm_get_char(self.get_thread_local_obj()) == 'L'

    def test_marker_1(self):
        self.start_transaction()
        p1 = stm_allocate(600)
        stm_set_char(p1, 'o')
        self.push_root(p1)
        self.push_root(ffi.cast("object_t *", 123))
        p2 = stm_allocate(600)
        stm_set_char(p2, 't')
        self.push_root(p2)
        stm_major_collect()
        assert lib._stm_total_allocated() == 2 * 616
        #
        p2 = self.pop_root()
        m = self.pop_root()
        assert m == ffi.cast("object_t *", 123)
        p1 = self.pop_root()
        assert stm_get_char(p1) == 'o'
        assert stm_get_char(p2) == 't'
