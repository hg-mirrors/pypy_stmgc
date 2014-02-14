from support import *
import py

class TestBasic(BaseTest):

    def test_empty(self):
        pass

    def test_thread_local_allocations(self):
        self.start_transaction()
        lp1 = stm_allocate(16)
        lp2 = stm_allocate(16)
        assert is_in_nursery(lp1)
        assert is_in_nursery(lp2)
        assert stm_get_real_address(lp2) - stm_get_real_address(lp1) == 16
        lp3 = stm_allocate(16)
        p3 = stm_get_real_address(lp3)
        assert p3 - stm_get_real_address(lp2) == 16
        #
        self.switch(1)
        self.start_transaction()
        lp1s = stm_allocate(16)
        assert is_in_nursery(lp1s)
        assert abs(stm_get_real_address(lp1s) - p3) >= 4000
        #
        self.switch(0)
        lp4 = stm_allocate(16)
        assert stm_get_real_address(lp4) - p3 == 16

    def test_transaction_start_stop(self):
        self.start_transaction()

        self.switch(1)
        self.start_transaction()
        self.commit_transaction()
        self.switch(0)

        self.commit_transaction()

    def test_simple_read(self):
        self.start_transaction()
        lp1 = stm_allocate(16)
        stm_read(lp1)
        assert stm_was_read(lp1)
        self.commit_transaction()

    def test_simple_write(self):
        self.start_transaction()
        lp1  = stm_allocate(16)
        assert stm_was_written(lp1)
        stm_write(lp1)
        assert stm_was_written(lp1)
        self.commit_transaction()

    def test_allocate_old(self):
        lp1 = stm_allocate_old(16)
        self.switch(1)
        lp2 = stm_allocate_old(16)
        assert lp1 != lp2

    def test_write_on_old(self):
        lp1 = stm_allocate_old(16)
        self.start_transaction()
        assert stm_get_char(lp1) == '\0'
        stm_write(lp1)
        assert stm_was_written(lp1)
        stm_set_char(lp1, 'a')
        assert stm_get_char(lp1) == 'a'

        self.switch(1)
        self.start_transaction()
        assert not stm_was_read(lp1)
        assert stm_get_char(lp1) == '\0'
        assert stm_was_read(lp1)
        assert stm_get_char(lp1) == '\0'
        self.commit_transaction()

    def test_read_write_1(self):
        lp1 = stm_allocate_old(16)
        stm_get_real_address(lp1)[HDR] = 'a' #setchar
        self.start_transaction()
        self.commit_transaction()
        #
        self.switch(1)
        self.start_transaction()
        stm_write(lp1)
        assert stm_get_char(lp1) == 'a'
        stm_set_char(lp1, 'b')
        #
        self.switch(0)
        self.start_transaction()
        stm_read(lp1)
        assert stm_get_char(lp1) == 'a'
        #
        self.switch(1)
        self.commit_transaction()
        #
        py.test.raises(Conflict, self.switch, 0) # detects rw conflict
        
    def test_commit_fresh_objects(self):
        self.start_transaction()
        lp = stm_allocate(16)
        stm_set_char(lp, 'u')
        p = stm_get_real_address(lp)
        stm_push_root(lp)
        self.commit_transaction()
        lp = stm_pop_root()
        p1 = stm_get_real_address(lp)
        assert p != p1
        
        self.switch(1)
        
        self.start_transaction()
        stm_write(lp) # privatize page
        p_ = stm_get_real_address(lp)
        assert p != p_
        assert p1 != p_
        assert stm_get_char(lp) == 'u'
        self.commit_transaction()

        
    def test_commit_fresh_objects2(self):
        self.switch(1)
        self.start_transaction()
        lp = stm_allocate(16)
        stm_set_char(lp, 'u')
        lp2 = stm_allocate(16)
        stm_set_char(lp2, 'v')
        assert stm_get_real_address(lp2) - stm_get_real_address(lp) == 16
        stm_write(lp) # test not crash
        stm_write(lp2) # test not crash
        stm_read(lp) # test not crash
        stm_read(lp2) # test not crash
        stm_push_root(lp)
        stm_push_root(lp2)
        self.commit_transaction()
        lp2 = stm_pop_root()
        lp = stm_pop_root()
        
        self.switch(0)
        
        self.start_transaction()
        stm_write(lp) # privatize page
        assert stm_get_char(lp) == 'u'
        stm_set_char(lp, 'x')
        stm_write(lp2)
        assert stm_get_char(lp2) == 'v'
        stm_set_char(lp2, 'y')
        self.commit_transaction()

        self.switch(1)

        self.start_transaction()
        stm_write(lp)
        assert stm_get_char(lp) == 'x'
        assert stm_get_char(lp2) == 'y'
        self.commit_transaction()

    def test_simple_refs(self):
        self.start_transaction()
        lp = stm_allocate_refs(3)
        lq = stm_allocate(16)
        lr = stm_allocate(16)
        stm_set_char(lq, 'x')
        stm_set_char(lr, 'y')
        stm_set_ref(lp, 0, lq)
        stm_set_ref(lp, 1, lr)
        stm_push_root(lp)
        self.commit_transaction()
        lp = stm_pop_root()

        self.switch(1)

        self.start_transaction()
        stm_write(lp)
        lq = stm_get_ref(lp, 0)
        lr = stm_get_ref(lp, 1)
        stm_read(lq)
        stm_read(lr)
        assert stm_get_char(lq) == 'x'
        assert stm_get_char(lr) == 'y'
        self.commit_transaction()


        
    def test_start_transaction_updates(self):
        self.start_transaction()
        lp1 = stm_allocate(16)
        stm_set_char(lp1, 'a')
        stm_push_root(lp1)
        self.commit_transaction()
        lp1 = stm_pop_root()
        #
        self.switch(1)
        self.start_transaction()
        stm_write(lp1)
        assert stm_get_char(lp1) == 'a'
        stm_set_char(lp1, 'b')
        self.commit_transaction()
        #
        self.switch(0)
        self.start_transaction()
        assert stm_get_char(lp1) == 'b'
        

    def test_resolve_no_conflict_empty(self):
        self.start_transaction()
        #
        self.switch(1)
        self.start_transaction()
        self.commit_transaction()
        #
        self.switch(0)
        self.commit_transaction()

    def test_resolve_no_conflict_write_only_in_already_committed(self):
        self.start_transaction()
        lp1 = stm_allocate(16)
        p1 = stm_get_real_address(lp1)
        p1[HDR] = 'a'
        stm_push_root(lp1)
        self.commit_transaction()
        lp1 = stm_pop_root()
        # 'a' in SHARED_PAGE
        
        self.start_transaction()
        
        self.switch(1)
        
        self.start_transaction()
        stm_write(lp1) # privatize page
        p1 = stm_get_real_address(lp1)
        assert p1[HDR] == 'a'
        p1[HDR] = 'b'
        self.commit_transaction()
        # 'b' both private pages
        #
        self.switch(0)
        #
        assert p1[HDR] == 'b'
        p1 = stm_get_real_address(lp1)
        assert p1[HDR] == 'b'
        self.commit_transaction()
        assert p1[HDR] == 'b'

    def test_not_resolve_write_read_conflict(self):
        self.start_transaction()
        lp1 = stm_allocate(16)
        stm_set_char(lp1, 'a')
        stm_push_root(lp1)
        self.commit_transaction()
        lp1 = stm_pop_root()
        
        self.start_transaction()
        stm_read(lp1)
        #
        self.switch(1)
        self.start_transaction()
        stm_write(lp1)
        stm_set_char(lp1, 'b')
        self.commit_transaction()
        #
        py.test.raises(Conflict, self.switch, 0)
        self.start_transaction()
        assert stm_get_char(lp1) == 'b'

    def test_resolve_write_read_conflict(self):
        self.start_transaction()
        lp1 = stm_allocate(16)
        stm_set_char(lp1, 'a')
        stm_push_root(lp1)
        self.commit_transaction()
        lp1 = stm_pop_root()
        
        self.start_transaction()
        #
        self.switch(1)
        self.start_transaction()
        stm_write(lp1)
        stm_set_char(lp1, 'b')
        self.commit_transaction()
        #
        self.switch(0)
        assert stm_get_char(lp1) == 'b'

    def test_resolve_write_write_conflict(self):
        self.start_transaction()
        lp1 = stm_allocate(16)
        stm_set_char(lp1, 'a')
        stm_push_root(lp1)
        self.commit_transaction()
        lp1 = stm_pop_root()
        
        self.start_transaction()
        stm_write(lp1) # acquire lock
        #
        self.switch(1)
        self.start_transaction()
        py.test.raises(Conflict, stm_write, lp1) # write-write conflict

    def test_abort_cleanup(self):
        self.start_transaction()
        lp1 = stm_allocate(16)
        stm_set_char(lp1, 'a')
        stm_push_root(lp1)
        self.commit_transaction()
        lp1 = stm_pop_root()

        self.start_transaction()
        stm_set_char(lp1, 'x')
        assert stm_abort_transaction()

        self.start_transaction()
        assert stm_get_char(lp1) == 'a'

    def test_many_allocs(self):
        obj_size = 1024
        num = (lib.NB_NURSERY_PAGES * 4096) / obj_size + 100 # more than what fits in the nursery
        
        self.start_transaction()
        for i in range(num):
            new = stm_allocate(obj_size)
            stm_push_root(new)

        old = []
        young = []
        for _ in range(num):
            r = stm_pop_root()
            if is_in_nursery(r):
                young.append(r)
            else:
                old.append(r)
                
        assert old
        assert young

    def test_larger_than_section(self):
        obj_size = lib.NURSERY_SECTION + 16
        
        self.start_transaction()
        new = stm_allocate(obj_size)
        assert not is_in_nursery(new)
        

    def test_large_obj_alloc(self):
        # test obj which doesn't fit into the size_classes
        # for now, we will still allocate it in the nursery.
        # expects: LARGE_OBJECT_WORDS  36
        size_class = 1000 # too big
        obj_size = size_class * 8
        assert obj_size > 4096 # we want more than 1 page
        assert obj_size < 4096 * 1024 # in the nursery

        self.start_transaction()
        new = stm_allocate(obj_size)
        assert is_in_nursery(new)
        assert len(stm_get_obj_pages(new)) == 2
        assert ([stm_get_page_flag(p) for p in stm_get_obj_pages(new)]
                == [lib.PRIVATE_PAGE]*2)
        stm_push_root(new)
        stm_minor_collect()
        new = stm_pop_root()

        assert len(stm_get_obj_pages(new)) == 2
        # assert ([stm_get_page_flag(p) for p in stm_get_obj_pages(new)]
        #         == [lib.UNCOMMITTED_SHARED_PAGE]*2)

        assert not is_in_nursery(new)

    def test_large_obj_write(self):
        # test obj which doesn't fit into the size_classes
        # expects: LARGE_OBJECT_WORDS  36
        size_class = 1000 # too big
        obj_size = size_class * 8
        assert obj_size > 4096 # we want more than 1 page
        assert obj_size < 4096 * 1024 # in the nursery

        self.start_transaction()
        new = stm_allocate(obj_size)
        assert is_in_nursery(new)
        stm_push_root(new)
        self.commit_transaction()
        new = stm_pop_root()

        assert ([stm_get_page_flag(p) for p in stm_get_obj_pages(new)]
                == [lib.SHARED_PAGE]*2)

        self.start_transaction()
        stm_write(new)
        assert ([stm_get_page_flag(p) for p in stm_get_obj_pages(new)]
                == [lib.PRIVATE_PAGE]*2)
        
        # write to 2nd page of object!!
        wnew = stm_get_real_address(new)
        wnew[4097] = 'x'

        self.switch(1)
        self.start_transaction()
        stm_read(new)
        rnew = stm_get_real_address(new)
        assert rnew[4097] == '\0'
        
    def test_partial_alloced_pages(self):
        self.start_transaction()
        new = stm_allocate(16)
        stm_push_root(new)
        stm_minor_collect()
        new = stm_pop_root()
        # assert stm_get_page_flag(stm_get_obj_pages(new)[0]) == lib.UNCOMMITTED_SHARED_PAGE
        # assert not (stm_get_flags(new) & lib.GCFLAG_NOT_COMMITTED)

        self.commit_transaction()
        assert stm_get_page_flag(stm_get_obj_pages(new)[0]) == lib.SHARED_PAGE
        assert not (stm_get_flags(new) & lib.GCFLAG_NOT_COMMITTED)

        self.start_transaction()
        newer = stm_allocate(16)
        stm_push_root(newer)
        stm_minor_collect()
        newer = stm_pop_root()
        # 'new' is still in shared_page and committed
        assert stm_get_page_flag(stm_get_obj_pages(new)[0]) == lib.SHARED_PAGE
        assert not (stm_get_flags(new) & lib.GCFLAG_NOT_COMMITTED)
        # 'newer' is now part of the SHARED page with 'new', but
        # marked as UNCOMMITTED, so no privatization has to take place:
        assert stm_get_obj_pages(new) == stm_get_obj_pages(newer)
        assert stm_get_flags(newer) & lib.GCFLAG_NOT_COMMITTED
        stm_write(newer) # does not privatize
        assert stm_get_page_flag(stm_get_obj_pages(newer)[0]) == lib.SHARED_PAGE
        self.commit_transaction()
        
        assert stm_get_page_flag(stm_get_obj_pages(newer)[0]) == lib.SHARED_PAGE
        assert not (stm_get_flags(newer) & lib.GCFLAG_NOT_COMMITTED)
        
    def test_reset_partial_alloc_pages(self):
        self.start_transaction()
        new = stm_allocate(16)
        stm_set_char(new, 'a')
        stm_push_root(new)
        stm_minor_collect()
        new = stm_pop_root()
        stm_abort_transaction()

        self.start_transaction()
        newer = stm_allocate(16)
        stm_push_root(newer)
        stm_minor_collect()
        newer = stm_pop_root()
        assert stm_get_real_address(new) == stm_get_real_address(newer)
        assert stm_get_char(newer) == '\0'

    def test_reuse_page(self):
        self.start_transaction()
        new = stm_allocate(16)
        stm_push_root(new)
        stm_minor_collect()
        new = stm_pop_root()
        # assert stm_get_page_flag(stm_get_obj_pages(new)[0]) == lib.UNCOMMITTED_SHARED_PAGE
        stm_abort_transaction()

        self.start_transaction()
        newer = stm_allocate(16)
        stm_push_root(newer)
        stm_minor_collect()
        newer = stm_pop_root()
        assert new == newer

    def test_write_to_old_after_minor(self):
        self.start_transaction()
        new = stm_allocate(16)
        stm_push_root(new)
        stm_minor_collect()
        old = stm_pop_root()
        self.commit_transaction()

        self.start_transaction()
        stm_write(old) # old objs to trace
        stm_set_char(old, 'x')
        stm_minor_collect()
        stm_write(old) # old objs to trace
        stm_set_char(old, 'y')
        self.commit_transaction()
        

    def test_inevitable_transaction(self):
        py.test.skip("stm_write and self.commit_transaction"
                     " of an inevitable tr. is not testable"
                     " since they wait for the other thread"
                     " to synchronize and possibly abort")

        old = stm_allocate_old(16)
        self.start_transaction()

        self.switch(1)
        self.start_transaction()
        stm_write(old)

        self.switch(0)
        stm_become_inevitable()
        stm_write(old) # t1 needs to abort, not us
        self.commit_transaction()

        py.test.raises(Conflict, self.switch, 1)
        
    # def test_resolve_write_write_no_conflict(self):
    #     self.start_transaction()
    #     p1 = stm_allocate(16)
    #     p2 = stm_allocate(16)
    #     p1[8] = 'a'
    #     p2[8] = 'A'
    #     self.commit_transaction(False)
    #     self.start_transaction()
    #     #
    #     self.switch(1)
    #     self.start_transaction()
    #     stm_write(p1)
    #     p1[8] = 'b'
    #     self.commit_transaction(False)
    #     #
    #     self.switch(0)
    #     stm_write(p2)
    #     p2[8] = 'C'
    #     self.commit_transaction(False)
    #     assert p1[8] == 'b'
    #     assert p2[8] == 'C'

    # def test_page_extra_malloc_unchanged_page(self):
    #     self.start_transaction()
    #     p1 = stm_allocate(16)
    #     p2 = stm_allocate(16)
    #     p1[8] = 'A'
    #     p2[8] = 'a'
    #     self.commit_transaction(False)
    #     self.start_transaction()
    #     #
    #     self.switch(1)
    #     self.start_transaction()
    #     stm_write(p1)
    #     assert p1[8] == 'A'
    #     p1[8] = 'B'
    #     self.commit_transaction(False)
    #     #
    #     self.switch(0)
    #     stm_read(p2)
    #     assert p2[8] == 'a'
    #     p3 = stm_allocate(16)   # goes into the same page, which is
    #     p3[8] = ':'             #  not otherwise modified
    #     self.commit_transaction(False)
    #     #
    #     assert p1[8] == 'B'
    #     assert p2[8] == 'a'
    #     assert p3[8] == ':'

    # def test_page_extra_malloc_changed_page_before(self):
    #     self.start_transaction()
    #     p1 = stm_allocate(16)
    #     p2 = stm_allocate(16)
    #     p1[8] = 'A'
    #     p2[8] = 'a'
    #     self.commit_transaction(False)
    #     self.start_transaction()
    #     #
    #     self.switch(1)
    #     self.start_transaction()
    #     stm_write(p1)
    #     assert p1[8] == 'A'
    #     p1[8] = 'B'
    #     self.commit_transaction(False)
    #     #
    #     self.switch(0)
    #     stm_write(p2)
    #     assert p2[8] == 'a'
    #     p2[8] = 'b'
    #     p3 = stm_allocate(16)  # goes into the same page, which I already
    #     p3[8] = ':'            #  modified just above
    #     self.commit_transaction(False)
    #     #
    #     assert p1[8] == 'B'
    #     assert p2[8] == 'b'
    #     assert p3[8] == ':'

    # def test_page_extra_malloc_changed_page_after(self):
    #     self.start_transaction()
    #     p1 = stm_allocate(16)
    #     p2 = stm_allocate(16)
    #     p1[8] = 'A'
    #     p2[8] = 'a'
    #     self.commit_transaction(False)
    #     self.start_transaction()
    #     #
    #     self.switch(1)
    #     self.start_transaction()
    #     stm_write(p1)
    #     assert p1[8] == 'A'
    #     p1[8] = 'B'
    #     self.commit_transaction(False)
    #     #
    #     self.switch(0)
    #     p3 = stm_allocate(16)  # goes into the same page, which I will
    #     p3[8] = ':'            #  modify just below
    #     stm_write(p2)
    #     assert p2[8] == 'a'
    #     p2[8] = 'b'
    #     self.commit_transaction(False)
    #     #
    #     assert p1[8] == 'B'
    #     assert p2[8] == 'b'
    #     assert p3[8] == ':'

    # def test_overflow_write_history(self):
    #     self.start_transaction()
    #     plist = [stm_allocate(n) for n in range(16, 256, 8)]
    #     self.commit_transaction(False)
    #     #
    #     for i in range(20):
    #         self.start_transaction()
    #         for p in plist:
    #             stm_write(p)
    #         self.commit_transaction(False)
