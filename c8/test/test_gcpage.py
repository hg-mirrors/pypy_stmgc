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

        pages = stm_get_obj_pages(new)
        assert len(pages) == 2
        assert ([stm_is_accessible_page(p) for p in pages]
                == [1, 1])

        assert not is_in_nursery(new)
        stm_write(new)
        self.commit_transaction()

        # now proceed to write into the object in a new transaction
        self.start_transaction()
        assert ([stm_is_accessible_page(p) for p in pages]
                == [True, True])
        stm_write(new)
        assert ([stm_is_accessible_page(p) for p in pages]
                == [True, True])

        # write to 2nd page of object!!
        wnew = stm_get_real_address(new)
        wnew[4097] = 'x'

        self.switch(1)
        self.start_transaction()
        assert ([stm_is_accessible_page(p) for p in pages]
                == [False, False])
        stm_read(new)
        rnew = stm_get_real_address(new)
        assert rnew[4097] == '\0'
        assert ([stm_is_accessible_page(p) for p in pages]
                == [False, True])
        self.abort_transaction()

        self.switch(0)
        self.abort_transaction()
        assert ([stm_is_accessible_page(p) for p in pages]
                == [True, True])


    def test_partial_alloced_pages(self):
        self.start_transaction()
        new = stm_allocate(16)
        self.push_root(new)
        stm_minor_collect()
        new = self.pop_root()

        pages = stm_get_obj_pages(new)
        assert stm_is_accessible_page(pages[0]) == True
        assert stm_get_flags(new) & GCFLAG_WRITE_BARRIER

        stm_set_char(new, 'x')
        assert not (stm_get_flags(new) & GCFLAG_WRITE_BARRIER)

        self.commit_transaction()

        #######

        assert stm_is_accessible_page(pages[0]) == True
        assert stm_get_flags(new) & GCFLAG_WRITE_BARRIER

        self.start_transaction()
        assert stm_get_char(new) == 'x'
        newer = stm_allocate(16)
        self.push_root(newer)
        stm_minor_collect()
        newer = self.pop_root()
        pageser = stm_get_obj_pages(newer)

        assert stm_get_flags(new) & GCFLAG_WRITE_BARRIER
        # same page as committed obj
        assert pages == pageser
        assert stm_get_flags(newer) & GCFLAG_WRITE_BARRIER

        stm_set_char(newer, 'y')
        assert not (stm_get_flags(newer) & GCFLAG_WRITE_BARRIER)
        self.commit_transaction()

        #####################

        self.switch(1)

        self.start_transaction()
        assert stm_is_accessible_page(pages[0]) == False
        assert stm_get_char(new) == 'x'
        assert stm_get_char(newer) == 'y'
        assert stm_is_accessible_page(pages[0]) == True

        another = stm_allocate(16)
        self.push_root(another)
        stm_minor_collect()
        another = self.pop_root()
        # segment has its own small-obj-pages:
        assert stm_get_obj_pages(another) != pages

        self.commit_transaction()


    def test_major_collection(self):
        self.start_transaction()
        new = stm_allocate(5000)
        self.push_root(new)
        stm_minor_collect()
        assert lib._stm_total_allocated() == 5000 + LMO

        new = self.pop_root()
        assert not is_in_nursery(new)
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

        py.test.skip("we don't account for private pages right now")
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
        assert not is_in_nursery(x)
        #
        self.switch(1 - invert)
        self.start_transaction()
        self.push_root(x)
        stm_set_char(x, 'A')
        stm_major_collect()

        py.test.skip("we don't account for private pages right now")
        assert lib._stm_total_allocated() == 5000 + LMO + 2 * 4096  # 2 pages
        self.commit_transaction()
        #
        self.start_transaction()
        stm_major_collect()
        assert lib._stm_total_allocated() == 5000 + LMO    # shared again

    def test_reshare_if_no_longer_modified_1(self):
        self.test_reshare_if_no_longer_modified_0(invert=1)

    def test_threadlocal_at_start_of_transaction(self):
        py.test.skip("no threadlocal right now")
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

    def test_keepalive_prebuilt(self):
        stm_allocate_old(64)
        self.start_transaction()
        assert lib._stm_total_allocated() == 64 + LMO # large malloc'd
        stm_major_collect()
        assert lib._stm_total_allocated() == 64 + LMO # large malloc'd
        self.commit_transaction()
