from support import *
import py

class TestBasic(BaseTest):

    def test_empty(self):
        pass

    def test_thread_local_allocations(self):
        stm_start_transaction()
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
        stm_start_transaction()
        lp1s = stm_allocate(16)
        assert is_in_nursery(lp1s)
        assert abs(stm_get_real_address(lp1s) - p3) >= 4000
        #
        self.switch(0)
        lp4 = stm_allocate(16)
        assert stm_get_real_address(lp4) - p3 == 16

    def test_transaction_start_stop(self):
        stm_start_transaction()
        
        self.switch(1)
        stm_start_transaction()
        stm_stop_transaction()
        self.switch(0)
        
        stm_stop_transaction()

    def test_simple_read(self):
        stm_start_transaction()
        lp1 = stm_allocate(16)
        stm_read(lp1)
        assert stm_was_read(lp1)
        stm_stop_transaction()

    def test_simple_write(self):
        stm_start_transaction()
        lp1  = stm_allocate(16)
        assert stm_was_written(lp1)
        stm_write(lp1)
        assert stm_was_written(lp1)
        stm_stop_transaction()

    def test_allocate_old(self):
        lp1 = stm_allocate_old(16)
        self.switch(1)
        lp2 = stm_allocate_old(16)
        assert lp1 != lp2
        
    def test_write_on_old(self):
        lp1 = stm_allocate_old(16)
        stm_start_transaction()
        stm_write(lp1)
        assert stm_was_written(lp1)
        stm_set_char(lp1, 'a')
        
        self.switch(1)
        stm_start_transaction()
        stm_read(lp1)
        assert stm_was_read(lp1)
        assert stm_get_char(lp1) == '\0'
        stm_stop_transaction()
        
        
    def test_read_write_1(self):
        lp1 = stm_allocate_old(16)
        stm_get_real_address(lp1)[HDR] = 'a' #setchar
        stm_start_transaction()
        stm_stop_transaction()
        #
        self.switch(1)
        stm_start_transaction()
        stm_write(lp1)
        assert stm_get_char(lp1) == 'a'
        stm_set_char(lp1, 'b')
        #
        self.switch(0)
        stm_start_transaction()
        stm_read(lp1)
        assert stm_get_char(lp1) == 'a'
        #
        self.switch(1)
        stm_stop_transaction()
        #
        py.test.raises(Conflict, self.switch, 0) # detects rw conflict
        
    def test_commit_fresh_objects(self):
        stm_start_transaction()
        lp = stm_allocate(16)
        stm_set_char(lp, 'u')
        p = stm_get_real_address(lp)
        stm_push_root(lp)
        stm_stop_transaction()
        lp = stm_pop_root()
        p1 = stm_get_real_address(lp)
        assert p != p1
        
        self.switch(1)
        
        stm_start_transaction()
        stm_write(lp) # privatize page
        p_ = stm_get_real_address(lp)
        assert p != p_
        assert p1 != p_
        assert stm_get_char(lp) == 'u'
        stm_stop_transaction()

        
    def test_commit_fresh_objects2(self):
        self.switch(1)
        stm_start_transaction()
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
        stm_stop_transaction()
        lp2 = stm_pop_root()
        lp = stm_pop_root()
        
        self.switch(0)
        
        stm_start_transaction()
        stm_write(lp) # privatize page
        assert stm_get_char(lp) == 'u'
        stm_set_char(lp, 'x')
        stm_write(lp2)
        assert stm_get_char(lp2) == 'v'
        stm_set_char(lp2, 'y')
        stm_stop_transaction()

        self.switch(1)

        stm_start_transaction()
        stm_write(lp)
        assert stm_get_char(lp) == 'x'
        assert stm_get_char(lp2) == 'y'
        stm_stop_transaction()

    def test_simple_refs(self):
        stm_start_transaction()
        lp = stm_allocate_refs(3)
        lq = stm_allocate(16)
        lr = stm_allocate(16)
        stm_set_char(lq, 'x')
        stm_set_char(lr, 'y')
        stm_set_ref(lp, 0, lq)
        stm_set_ref(lp, 1, lr)
        stm_push_root(lp)
        stm_stop_transaction()
        lp = stm_pop_root()

        self.switch(1)

        stm_start_transaction()
        stm_write(lp)
        lq = stm_get_ref(lp, 0)
        lr = stm_get_ref(lp, 1)
        stm_read(lq)
        stm_read(lr)
        assert stm_get_char(lq) == 'x'
        assert stm_get_char(lr) == 'y'
        stm_stop_transaction()


        
    def test_start_transaction_updates(self):
        stm_start_transaction()
        lp1 = stm_allocate(16)
        stm_set_char(lp1, 'a')
        stm_push_root(lp1)
        stm_stop_transaction()
        lp1 = stm_pop_root()
        #
        self.switch(1)
        stm_start_transaction()
        stm_write(lp1)
        assert stm_get_char(lp1) == 'a'
        stm_set_char(lp1, 'b')
        stm_stop_transaction()
        #
        self.switch(0)
        stm_start_transaction()
        assert stm_get_char(lp1) == 'b'
        

    def test_resolve_no_conflict_empty(self):
        stm_start_transaction()
        #
        self.switch(1)
        stm_start_transaction()
        stm_stop_transaction()
        #
        self.switch(0)
        stm_stop_transaction()

    def test_resolve_no_conflict_write_only_in_already_committed(self):
        stm_start_transaction()
        lp1 = stm_allocate(16)
        p1 = stm_get_real_address(lp1)
        p1[HDR] = 'a'
        stm_push_root(lp1)
        stm_stop_transaction()
        lp1 = stm_pop_root()
        # 'a' in SHARED_PAGE
        
        stm_start_transaction()
        
        self.switch(1)
        
        stm_start_transaction()
        stm_write(lp1) # privatize page
        p1 = stm_get_real_address(lp1)
        assert p1[HDR] == 'a'
        p1[HDR] = 'b'
        stm_stop_transaction()
        # 'b' both private pages
        #
        self.switch(0)
        #
        assert p1[HDR] == 'b'
        p1 = stm_get_real_address(lp1)
        assert p1[HDR] == 'b'
        stm_stop_transaction()
        assert p1[HDR] == 'b'

    def test_not_resolve_write_read_conflict(self):
        stm_start_transaction()
        lp1 = stm_allocate(16)
        stm_set_char(lp1, 'a')
        stm_push_root(lp1)
        stm_stop_transaction()
        lp1 = stm_pop_root()
        
        stm_start_transaction()
        stm_read(lp1)
        #
        self.switch(1)
        stm_start_transaction()
        stm_write(lp1)
        stm_set_char(lp1, 'b')
        stm_stop_transaction()
        #
        py.test.raises(Conflict, self.switch, 0)
        stm_start_transaction()
        assert stm_get_char(lp1) == 'b'

    def test_resolve_write_read_conflict(self):
        stm_start_transaction()
        lp1 = stm_allocate(16)
        stm_set_char(lp1, 'a')
        stm_push_root(lp1)
        stm_stop_transaction()
        lp1 = stm_pop_root()
        
        stm_start_transaction()
        #
        self.switch(1)
        stm_start_transaction()
        stm_write(lp1)
        stm_set_char(lp1, 'b')
        stm_stop_transaction()
        #
        self.switch(0)
        assert stm_get_char(lp1) == 'b'

    def test_resolve_write_write_conflict(self):
        stm_start_transaction()
        lp1 = stm_allocate(16)
        stm_set_char(lp1, 'a')
        stm_push_root(lp1)
        stm_stop_transaction()
        lp1 = stm_pop_root()
        
        stm_start_transaction()
        stm_write(lp1) # acquire lock
        #
        self.switch(1)
        stm_start_transaction()
        py.test.raises(Conflict, stm_write, lp1) # write-write conflict

    def test_abort_cleanup(self):
        stm_start_transaction()
        lp1 = stm_allocate(16)
        stm_set_char(lp1, 'a')
        stm_push_root(lp1)
        stm_stop_transaction()
        lp1 = stm_pop_root()

        stm_start_transaction()
        stm_set_char(lp1, 'x')
        assert stm_abort_transaction()

        stm_start_transaction()
        assert stm_get_char(lp1) == 'a'

    def test_many_allocs(self):
        # assumes NB_NURSERY_PAGES    1024
        obj_size = 1024
        num = 5000 # more than what fits in the nursery (4MB)
        
        stm_start_transaction()
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

    def test_large_obj_alloc(self):
        # test obj which doesn't fit into the size_classes
        # for now, we will still allocate it in the nursery.
        # expects: LARGE_OBJECT_WORDS  36
        size_class = 1000 # too big
        obj_size = size_class * 8
        assert obj_size > 4096 # we want more than 1 page
        assert obj_size < 4096 * 1024 # in the nursery

        stm_start_transaction()
        new = stm_allocate(obj_size)
        assert is_in_nursery(new)
        assert len(stm_get_obj_pages(new)) == 2
        assert ([stm_get_page_flag(p) for p in stm_get_obj_pages(new)]
                == [lib.PRIVATE_PAGE]*2)
        stm_push_root(new)
        stm_minor_collect()
        new = stm_pop_root()

        assert len(stm_get_obj_pages(new)) == 2
        assert ([stm_get_page_flag(p) for p in stm_get_obj_pages(new)]
                == [lib.UNCOMMITTED_SHARED_PAGE]*2)

        assert not is_in_nursery(new)

    def test_large_obj_write(self):
        # test obj which doesn't fit into the size_classes
        # expects: LARGE_OBJECT_WORDS  36
        size_class = 1000 # too big
        obj_size = size_class * 8
        assert obj_size > 4096 # we want more than 1 page
        assert obj_size < 4096 * 1024 # in the nursery

        stm_start_transaction()
        new = stm_allocate(obj_size)
        assert is_in_nursery(new)
        stm_push_root(new)
        stm_stop_transaction()
        new = stm_pop_root()

        assert ([stm_get_page_flag(p) for p in stm_get_obj_pages(new)]
                == [lib.SHARED_PAGE]*2)

        stm_start_transaction()
        stm_write(new)
        assert ([stm_get_page_flag(p) for p in stm_get_obj_pages(new)]
                == [lib.PRIVATE_PAGE]*2)
        
        # write to 2nd page of object!!
        wnew = stm_get_real_address(new)
        wnew[4097] = 'x'

        self.switch(1)
        stm_start_transaction()
        stm_read(new)
        rnew = stm_get_real_address(new)
        assert rnew[4097] == '\0'
        

            


    # def test_resolve_write_write_no_conflict(self):
    #     stm_start_transaction()
    #     p1 = stm_allocate(16)
    #     p2 = stm_allocate(16)
    #     p1[8] = 'a'
    #     p2[8] = 'A'
    #     stm_stop_transaction(False)
    #     stm_start_transaction()
    #     #
    #     self.switch(1)
    #     stm_start_transaction()
    #     stm_write(p1)
    #     p1[8] = 'b'
    #     stm_stop_transaction(False)
    #     #
    #     self.switch(0)
    #     stm_write(p2)
    #     p2[8] = 'C'
    #     stm_stop_transaction(False)
    #     assert p1[8] == 'b'
    #     assert p2[8] == 'C'

    # def test_page_extra_malloc_unchanged_page(self):
    #     stm_start_transaction()
    #     p1 = stm_allocate(16)
    #     p2 = stm_allocate(16)
    #     p1[8] = 'A'
    #     p2[8] = 'a'
    #     stm_stop_transaction(False)
    #     stm_start_transaction()
    #     #
    #     self.switch(1)
    #     stm_start_transaction()
    #     stm_write(p1)
    #     assert p1[8] == 'A'
    #     p1[8] = 'B'
    #     stm_stop_transaction(False)
    #     #
    #     self.switch(0)
    #     stm_read(p2)
    #     assert p2[8] == 'a'
    #     p3 = stm_allocate(16)   # goes into the same page, which is
    #     p3[8] = ':'             #  not otherwise modified
    #     stm_stop_transaction(False)
    #     #
    #     assert p1[8] == 'B'
    #     assert p2[8] == 'a'
    #     assert p3[8] == ':'

    # def test_page_extra_malloc_changed_page_before(self):
    #     stm_start_transaction()
    #     p1 = stm_allocate(16)
    #     p2 = stm_allocate(16)
    #     p1[8] = 'A'
    #     p2[8] = 'a'
    #     stm_stop_transaction(False)
    #     stm_start_transaction()
    #     #
    #     self.switch(1)
    #     stm_start_transaction()
    #     stm_write(p1)
    #     assert p1[8] == 'A'
    #     p1[8] = 'B'
    #     stm_stop_transaction(False)
    #     #
    #     self.switch(0)
    #     stm_write(p2)
    #     assert p2[8] == 'a'
    #     p2[8] = 'b'
    #     p3 = stm_allocate(16)  # goes into the same page, which I already
    #     p3[8] = ':'            #  modified just above
    #     stm_stop_transaction(False)
    #     #
    #     assert p1[8] == 'B'
    #     assert p2[8] == 'b'
    #     assert p3[8] == ':'

    # def test_page_extra_malloc_changed_page_after(self):
    #     stm_start_transaction()
    #     p1 = stm_allocate(16)
    #     p2 = stm_allocate(16)
    #     p1[8] = 'A'
    #     p2[8] = 'a'
    #     stm_stop_transaction(False)
    #     stm_start_transaction()
    #     #
    #     self.switch(1)
    #     stm_start_transaction()
    #     stm_write(p1)
    #     assert p1[8] == 'A'
    #     p1[8] = 'B'
    #     stm_stop_transaction(False)
    #     #
    #     self.switch(0)
    #     p3 = stm_allocate(16)  # goes into the same page, which I will
    #     p3[8] = ':'            #  modify just below
    #     stm_write(p2)
    #     assert p2[8] == 'a'
    #     p2[8] = 'b'
    #     stm_stop_transaction(False)
    #     #
    #     assert p1[8] == 'B'
    #     assert p2[8] == 'b'
    #     assert p3[8] == ':'

    # def test_overflow_write_history(self):
    #     stm_start_transaction()
    #     plist = [stm_allocate(n) for n in range(16, 256, 8)]
    #     stm_stop_transaction(False)
    #     #
    #     for i in range(20):
    #         stm_start_transaction()
    #         for p in plist:
    #             stm_write(p)
    #         stm_stop_transaction(False)
