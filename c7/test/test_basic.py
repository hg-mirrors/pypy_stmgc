from support import *


class TestBasic(BaseTest):

    def test_empty(self):
        pass

    def test_thread_local_allocations(self):
        lp1, p1 = stm_allocate(16)
        lp2, p2 = stm_allocate(16)
        assert is_in_nursery(p1)
        assert is_in_nursery(p2)
        assert p2 - p1 == 16
        lp3, p3 = stm_allocate(16)
        assert p3 - p2 == 16
        #
        self.switch(1)
        lp1s, p1s = stm_allocate(16)
        assert abs(p1s - p3) >= 4000
        #
        self.switch(0)
        lp4, p4 = stm_allocate(16)
        assert p4 - p3 == 16

    def test_transaction_start_stop(self):
        stm_start_transaction()
        
        self.switch(1)
        stm_start_transaction()
        stm_stop_transaction()
        self.switch(0)
        
        stm_stop_transaction()

    def test_simple_read(self):
        stm_start_transaction()
        lp1, _ = stm_allocate(16)
        stm_read(lp1)
        assert stm_was_read(lp1)
        stm_stop_transaction()

    def test_simple_write(self):
        stm_start_transaction()
        lp1, _  = stm_allocate(16)
        assert stm_was_written(lp1)
        stm_write(lp1)
        assert stm_was_written(lp1)
        stm_stop_transaction()

    def test_allocate_old(self):
        lp1, _ = stm_allocate_old(16)
        self.switch(1)
        lp2, _ = stm_allocate_old(16)
        assert lp1 != lp2
        
    def test_write_on_old(self):
        lp1, p1 = stm_allocate_old(16)
        stm_start_transaction()
        stm_write(lp1)
        assert stm_was_written(lp1)
        p1[15] = 'a'
        
        self.switch(1)
        stm_start_transaction()
        stm_read(lp1)
        assert stm_was_read(lp1)
        tp1 = stm_get_real_address(lp1)
        assert tp1[15] == '\0'
        stm_stop_transaction()
        self.switch(0)
        
        stm_stop_transaction()
        
    def test_read_write_1(self):
        lp1, p1 = stm_allocate_old(16)
        p1[8] = 'a'
        stm_start_transaction()
        stm_stop_transaction()
        #
        self.switch(1)
        stm_start_transaction()
        stm_write(lp1)
        p1 = stm_get_real_address(lp1)
        assert p1[8] == 'a'
        p1[8] = 'b'
        
        #
        self.switch(0)
        stm_start_transaction()
        stm_read(lp1)
        p1 = stm_get_real_address(lp1)
        assert p1[8] == 'a'
        #
        self.switch(1)
        stm_stop_transaction(False)
        #
        self.switch(0, expect_conflict=True) # detects rw conflict
        
    def test_commit_fresh_objects(self):
        stm_start_transaction()
        lp, p = stm_allocate(16)
        p[8] = 'u'
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
        assert p_[8] == 'u'
        stm_stop_transaction()

        
    def test_commit_fresh_objects2(self):
        self.switch(1)
        stm_start_transaction()
        lp, p = stm_allocate(16)
        p[8] = 'u'
        lp2, p2 = stm_allocate(16)
        p2[8] = 'v'
        assert p2 - p == 16
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
        p_ = stm_get_real_address(lp)
        assert p_[8] == 'u'
        p_[8] = 'x'
        stm_write(lp2)
        p2_ = stm_get_real_address(lp2)
        assert p2_[8] == 'v'
        p2_[8] = 'y'
        stm_stop_transaction()

        self.switch(1)

        stm_start_transaction()
        stm_write(lp)
        p_ = stm_get_real_address(lp)
        assert p_[8] == 'x'
        stm_read(lp2)
        p2_ = stm_get_real_address(lp2)
        assert p2_[8] == 'y'
        stm_stop_transaction()

    def test_simple_refs(self):
        stm_start_transaction()
        lp, p = stm_allocate_refs(3)
        lq, q = stm_allocate(16)
        lr, r = stm_allocate(16)
        q[8] = 'x'
        r[8] = 'y'
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
        assert stm_get_real_address(lq)[8] == 'x'
        assert stm_get_real_address(lr)[8] == 'y'
        stm_stop_transaction()


        
    # def test_read_write_2(self):
    #     stm_start_transaction()
    #     lp1, p1 = stm_allocate(16)
    #     p1[8] = 'a'
    #     stm_stop_transaction(False)
    #     #
    #     self.switch(1)
    #     stm_start_transaction()
    #     stm_write(lp1)
    #     p1 = stm_get_real_address(lp1)
    #     assert p1[8] == 'a'
    #     p1[8] = 'b'
    #     #
    #     self.switch(0)
    #     stm_start_transaction()
    #     stm_read(lp1)
    #     p1 = stm_get_real_address(lp1)
    #     assert p1[8] == 'a'
    #     #
    #     self.switch(1)
    #     stm_stop_transaction(False)
    #     #
    #     self.switch(0)
    #     p1 = stm_get_real_address(lp1)
    #     assert p1[8] == 'a'

        
    # def test_start_transaction_updates(self):
    #     stm_start_transaction()
    #     p1 = stm_allocate(16)
    #     p1[8] = 'a'
    #     stm_stop_transaction(False)
    #     #
    #     self.switch(1)
    #     stm_start_transaction()
    #     stm_write(p1)
    #     assert p1[8] == 'a'
    #     p1[8] = 'b'
    #     stm_stop_transaction(False)
    #     #
    #     self.switch(0)
    #     assert p1[8] == 'a'
    #     stm_start_transaction()
    #     assert p1[8] == 'b'

    # def test_resolve_no_conflict_empty(self):
    #     stm_start_transaction()
    #     #
    #     self.switch(1)
    #     stm_start_transaction()
    #     stm_stop_transaction(False)
    #     #
    #     self.switch(0)
    #     stm_stop_transaction(False)

    # def test_resolve_no_conflict_write_only_in_already_committed(self):
    #     stm_start_transaction()
    #     p1 = stm_allocate(16)
    #     p1[8] = 'a'
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
    #     assert p1[8] == 'a'
    #     stm_stop_transaction(False)
    #     assert p1[8] == 'b'

    # def test_resolve_write_read_conflict(self):
    #     stm_start_transaction()
    #     p1 = stm_allocate(16)
    #     p1[8] = 'a'
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
    #     stm_read(p1)
    #     assert p1[8] == 'a'
    #     stm_stop_transaction(expected_conflict=True)
    #     assert p1[8] in ('a', 'b')
    #     stm_start_transaction()
    #     assert p1[8] == 'b'

    # def test_resolve_write_write_conflict(self):
    #     stm_start_transaction()
    #     p1 = stm_allocate(16)
    #     p1[8] = 'a'
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
    #     assert p1[8] == 'a'
    #     stm_write(p1)
    #     p1[8] = 'c'
    #     stm_stop_transaction(expected_conflict=True)
    #     assert p1[8] in ('a', 'b')
    #     stm_start_transaction()
    #     assert p1[8] == 'b'

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
