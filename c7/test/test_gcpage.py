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
        assert ([stm_get_page_flag(p) for p in stm_get_obj_pages(new)]
                == [SHARED_PAGE]*2)

        assert not is_in_nursery(new)
        stm_write(new)
        self.commit_transaction()

        # now proceed to write into the object in a new transaction
        self.start_transaction()
        assert ([stm_get_page_flag(p) for p in stm_get_obj_pages(new)]
                == [SHARED_PAGE]*2)
        stm_write(new)
        assert ([stm_get_page_flag(p) for p in stm_get_obj_pages(new)]
                == [PRIVATE_PAGE]*2)

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
        assert ([stm_get_page_flag(p) for p in stm_get_obj_pages(new)]
                == [PRIVATE_PAGE]*2)

    def test_partial_alloced_pages(self):
        self.start_transaction()
        new = stm_allocate(16)
        self.push_root(new)
        stm_minor_collect()
        new = self.pop_root()

        assert stm_get_page_flag(stm_get_obj_pages(new)[0]) == SHARED_PAGE
        assert stm_get_flags(new) & GCFLAG_WRITE_BARRIER

        stm_write(new)
        assert not (stm_get_flags(new) & GCFLAG_WRITE_BARRIER)

        self.commit_transaction()
        assert stm_get_page_flag(stm_get_obj_pages(new)[0]) == SHARED_PAGE
        assert stm_get_flags(new) & GCFLAG_WRITE_BARRIER

        self.start_transaction()
        newer = stm_allocate(16)
        self.push_root(newer)
        stm_minor_collect()
        newer = self.pop_root()
        # 'new' is still in shared_page and committed
        assert stm_get_page_flag(stm_get_obj_pages(new)[0]) == SHARED_PAGE
        assert stm_get_flags(new) & GCFLAG_WRITE_BARRIER
        # 'newer' is now part of the SHARED page with 'new', but
        # uncommitted, so no privatization has to take place:
        assert stm_get_obj_pages(new) == stm_get_obj_pages(newer)
        assert stm_get_flags(newer) & GCFLAG_WRITE_BARRIER
        stm_write(newer) # does not privatize
        assert not (stm_get_flags(newer) & GCFLAG_WRITE_BARRIER)
        assert stm_get_page_flag(stm_get_obj_pages(newer)[0]) == SHARED_PAGE
        self.commit_transaction()

        assert stm_get_page_flag(stm_get_obj_pages(newer)[0]) == SHARED_PAGE
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
            return new

        self.start_transaction()
        self.push_root(make_chain(5000))
        self.push_root(make_chain(4312))
        stm_minor_collect()
        assert lib._stm_total_allocated() == (10 * (5000 + LMO) +
                                              10 * (4312 + LMO))
