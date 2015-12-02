from support import *
import py


LMO = LARGE_MALLOC_OVERHEAD
CLEO = COMMIT_LOG_ENTRY_OVERHEAD
CLEEO = COMMIT_LOG_ENTRY_ENTRY_OVERHEAD



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
        new = stm_allocate(5008)
        self.push_root(new)
        stm_minor_collect()
        assert lib._stm_total_allocated() == 5008 + LMO

        new = self.pop_root()
        assert not is_in_nursery(new)
        stm_minor_collect()
        assert lib._stm_total_allocated() == 5008 + LMO

        stm_major_collect()
        assert lib._stm_total_allocated() == 0

    def test_account_for_everything(self):
        self.start_transaction()
        self.commit_transaction()
        assert lib._stm_total_allocated() == 0
        assert lib._stm_cle_allocated() == CLEO

        self.start_transaction()
        o = stm_allocate(5008)
        self.push_root(o)
        self.commit_transaction()
        assert last_commit_log_entry_objs() == []
        # 2 CLEs, 1 old object
        assert lib._stm_total_allocated() == 5008 + LMO
        # however, on commit, we could free 1/2 CLE
        assert lib._stm_cle_allocated() == 1*CLEO

        self.start_transaction()
        o = self.pop_root()
        stm_set_char(o, 'x')
        self.push_root(o)
        self.commit_transaction()
        assert last_commit_log_entry_objs() == [o]*2
        # 2 CLEs, 1 old object
        # also, 2 slices of bk_copy and thus 2 CLE entries
        assert lib._stm_total_allocated() == 5008+LMO
        # however, on commit, we could free 1/2 CLE
        assert lib._stm_cle_allocated() == 1*CLEO + (5008 + CLEEO*2)

        self.start_transaction()
        assert lib._stm_total_allocated() == 5008+LMO
        assert lib._stm_cle_allocated() == 1*CLEO + (5008 + CLEEO*2)
        stm_major_collect()
        assert lib._stm_total_allocated() == (5008+LMO)
        assert lib._stm_cle_allocated() == 1*CLEO + (5008 + CLEEO*2)
        self.commit_transaction()


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
        self.push_root(make_chain(5008))
        self.push_root(make_chain(4304))
        stm_minor_collect()
        assert lib._stm_total_allocated() == (10 * (5008 + LMO) +
                                              10 * (4304 + LMO))
        stm_major_collect()
        assert lib._stm_total_allocated() == (10 * (5008 + LMO) +
                                              10 * (4304 + LMO))
        stm_major_collect()
        assert lib._stm_total_allocated() == (10 * (5008 + LMO) +
                                              10 * (4304 + LMO))
        self.pop_root()
        stm_major_collect()
        assert lib._stm_total_allocated() == 10 * (5008 + LMO)

    def test_trace_all_versions(self):
        self.start_transaction()
        x = stm_allocate(5008)
        stm_set_char(x, 'A')
        stm_set_char(x, 'a', 4999)
        self.push_root(x)
        self.commit_transaction()
        assert lib._stm_total_allocated() == 5008 + LMO
        assert lib._stm_cle_allocated() == CLEO

        self.start_transaction()
        x = self.pop_root()
        self.push_root(x)
        assert lib._stm_total_allocated() == 5008 + LMO
        assert lib._stm_cle_allocated() == CLEO
        stm_set_char(x, 'B')
        stm_set_char(x, 'b', 4999)

        py.test.skip("we don't account for private pages right now")
        assert lib._stm_total_allocated() == 5008 + LMO + 2 * 4096  # 2 pages
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
        assert lib._stm_total_allocated() == 5008 + LMO + 2 * 4096  # 2 pages

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
        self.test_trace_correct_version_of_overflow_objects_1(size=5008)

    def test_reshare_if_no_longer_modified_0(self, invert=0):
        if invert:
            self.switch(1)
        self.start_transaction()
        x = stm_allocate(5008)
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
        assert lib._stm_total_allocated() == 5008 + LMO + 2 * 4096  # 2 pages
        self.commit_transaction()
        #
        self.start_transaction()
        stm_major_collect()
        assert lib._stm_total_allocated() == 5008 + LMO    # shared again

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
        self.commit_transaction()

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
        assert lib._stm_total_allocated() == 2 * 624
        #
        p2 = self.pop_root()
        m = self.pop_root()
        assert m == ffi.cast("object_t *", 123)
        p1 = self.pop_root()
        assert stm_get_char(p1) == 'o'
        assert stm_get_char(p2) == 't'

    def test_keepalive_prebuilt(self):
        stm_allocate_old(64)
        big = GC_LAST_SMALL_SIZE+64
        stm_allocate_old(big)

        # see allocate_outside_nursery_large:
        actual_big = (big + 15 ) & ~15

        self.start_transaction()
        # 4096 for 1 page of smallmalloc:
        assert lib._stm_total_allocated() == 4096 + (actual_big + LMO) # large malloc'd
        stm_major_collect()
        assert lib._stm_total_allocated() == 4096 + (actual_big + LMO) # large malloc'd
        self.commit_transaction()

    def test_bug(self):
        lp_ref_4 = stm_allocate_old_refs(50)
        #
        self.start_transaction()
        stm_set_ref(lp_ref_4, 0, ffi.NULL, False)
        #
        self.switch(1)
        self.start_transaction()
        self.become_inevitable()
        #
        self.switch(0)
        py.test.raises(Conflict, self.commit_transaction)
        #
        self.switch(1)

        stm_set_ref(lp_ref_4, 0, ffi.NULL, False)

        self.commit_transaction()
        self.start_transaction()
        stm_major_collect()

    def test_bug2(self):
        lp_ref_4 = stm_allocate_old(16)
        #
        self.start_transaction()
        stm_set_char(lp_ref_4, 'x')
        #
        self.switch(1)
        self.start_transaction()
        stm_set_char(lp_ref_4, 'y')
        #
        self.switch(0)
        self.commit_transaction()
        self.start_transaction()
        stm_major_collect()
        stm_major_collect()
        stm_major_collect()
        #
        py.test.raises(Conflict, self.switch, 1)

    def test_small_major_collection(self):
        self.start_transaction()
        new = stm_allocate(16) # small
        assert lib._stm_total_allocated() == 0
        self.push_root(new)
        stm_minor_collect()
        assert lib._stm_total_allocated() == 4096

        new = self.pop_root()
        assert not is_in_nursery(new)
        stm_minor_collect()
        assert lib._stm_total_allocated() == 4096

        stm_major_collect()
        assert lib._stm_total_allocated() == 0

    def test_mixed_major_collections(self):
        import random
        obj_sizes = [1024, 1000*8]

        self.start_transaction()
        random.seed(123)

        # allocate objs:
        allocated = 0
        NOBJS = 100
        for _ in range(NOBJS):
            osize = random.choice(obj_sizes)
            allocated += osize + LMO

            o = stm_allocate(osize)
            self.push_root(o)

            # sometimes do a minor collection:
            if random.random() > 0.95:
                stm_minor_collect()
                assert lib._stm_total_allocated() == allocated

        stm_minor_collect()
        assert lib._stm_total_allocated() == allocated
        # -> all objs old

        objs = set()
        for _ in range(NOBJS):
            objs.add(self.pop_root())

        # do major collections while always saving less
        # and less objs
        while objs:
            objs = random.sample(objs, len(objs) // 2)
            for o in objs:
                self.push_root(o)
            stm_major_collect()
            for o in objs:
                self.pop_root()

        assert lib._stm_total_allocated() == 0

        self.commit_transaction()

    def test_abort_thread_doing_major_gc(self):

        o = stm_allocate_old(16)
        self.start_transaction()
        stm_set_char(o, 'a')

        self.switch(1)
        self.start_transaction()
        stm_set_char(o, 'b')

        self.switch(0)
        self.commit_transaction()

        self.switch(1, False)
        py.test.raises(Conflict, stm_major_collect)


    def test_cleaning_of_cl(self):
        self.start_transaction()
        stm_major_collect()
        self.commit_transaction()
        assert count_commit_log_entries() == 1

        self.switch(1)
        self.start_transaction()
        self.commit_transaction()
        assert count_commit_log_entries() == 1

        py.test.xfail("XXX: we never completely free the CLEs anymore")
        self.switch(0)
        self.start_transaction()
        stm_major_collect()
        assert count_commit_log_entries() == 0

        stm_major_collect()

        self.become_inevitable()
        stm_major_collect()
        stm_major_collect()
        self.commit_transaction()

    def test_small_old_surives_major(self):
        s = stm_allocate_old_small(16)
        self.start_transaction()
        stm_major_collect()
        assert stm_get_char(s) == '\0'
        self.commit_transaction()


    def test_overflow_on_ss_in_major_gc(self):
        self.start_transaction()
        o = stm_allocate_refs(100)
        p = stm_allocate(16)
        stm_set_ref(o, 0, p)
        self.push_root(o)
        stm_minor_collect()
        o = self.pop_root()
        p = stm_get_ref(o, 0)
        assert stm_get_char(p) == '\0'
        self.push_root(o)

        self.switch(1)

        self.start_transaction()
        stm_major_collect()
        self.commit_transaction()

        self.switch(0)
        # p not freed
        assert stm_get_char(p) == '\0'
        self.commit_transaction()
