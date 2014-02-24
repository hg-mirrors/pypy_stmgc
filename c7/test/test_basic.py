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
        assert modified_old_objects() == []             # object not old
        assert old_objects_pointing_to_nursery() == None    # short transac.
        assert overflow_objects_pointing_to_nursery() == None # short transac.
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
        assert modified_old_objects() == []
        stm_write(lp1)
        assert modified_old_objects() == [lp1]
        assert old_objects_pointing_to_nursery() == None
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
        self.push_root(lp)
        self.commit_transaction()
        lp = self.pop_root()
        p1 = stm_get_real_address(lp)

        self.switch(1)

        self.start_transaction()
        assert stm_get_char(lp) == 'u'
        stm_write(lp) # privatize page
        p2 = stm_get_real_address(lp)
        assert p1 != p2       # we see the other segment, but same object
        assert (p2 - p1) % 4096 == 0
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
        self.push_root(lp)
        self.push_root(lp2)
        self.commit_transaction()
        lp2 = self.pop_root()
        lp = self.pop_root()

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

    def test_commit_fresh_objects3(self):
        # make object lpx; then privatize the page by committing changes
        # to it; then create lpy in the same page.  Check that lpy is
        # visible from the other thread.
        self.start_transaction()
        lpx = stm_allocate(16)
        stm_set_char(lpx, '.')
        self.push_root(lpx)
        self.commit_transaction()
        lpx = self.pop_root()
        self.push_root(lpx)

        self.start_transaction()
        stm_set_char(lpx, 'X')
        self.commit_transaction()

        self.start_transaction()
        lpy = stm_allocate(16)
        stm_set_char(lpy, 'y')
        self.push_root(lpy)
        assert modified_old_objects() == []
        self.commit_transaction()
        lpy = self.pop_root()

        self.switch(1)
        self.start_transaction()
        assert stm_get_char(lpx) == 'X'
        assert stm_get_char(lpy) == 'y'

    def test_simple_refs(self):
        self.start_transaction()
        lp = stm_allocate_refs(3)
        lq = stm_allocate(16)
        lr = stm_allocate(16)
        stm_set_char(lq, 'x')
        stm_set_char(lr, 'y')
        stm_set_ref(lp, 0, lq)
        stm_set_ref(lp, 1, lr)
        self.push_root(lp)
        self.commit_transaction()
        lp = self.pop_root()

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
        self.push_root(lp1)
        self.commit_transaction()
        lp1 = self.pop_root()
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
        self.push_root(lp1)
        self.commit_transaction()
        lp1 = self.pop_root()
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

    def test_not_resolve_write_read_conflict_1(self):
        self.start_transaction()
        lp1 = stm_allocate(16)
        stm_set_char(lp1, 'a')
        self.push_root(lp1)
        self.commit_transaction()
        lp1 = self.pop_root()

        self.switch(1)
        self.start_transaction()
        #
        self.switch(0)
        self.start_transaction()
        stm_read(lp1)
        #
        self.switch(1)
        stm_write(lp1)
        stm_set_char(lp1, 'b')
        self.commit_transaction()
        #
        # transaction from thread 1 is older than from thread 0
        py.test.raises(Conflict, self.switch, 0)
        self.start_transaction()
        assert stm_get_char(lp1) == 'b'

    def test_not_resolve_write_read_conflict_2(self):
        self.start_transaction()
        lp1 = stm_allocate(16)
        stm_set_char(lp1, 'a')
        self.push_root(lp1)
        self.commit_transaction()
        lp1 = self.pop_root()
        
        self.start_transaction()
        stm_read(lp1)
        #
        self.switch(1)
        self.start_transaction()
        stm_write(lp1)
        stm_set_char(lp1, 'b')
        # transaction from thread 1 is newer than from thread 0
        py.test.raises(Conflict, self.commit_transaction)

    def test_resolve_write_read_conflict(self):
        self.start_transaction()
        lp1 = stm_allocate(16)
        stm_set_char(lp1, 'a')
        self.push_root(lp1)
        self.commit_transaction()
        lp1 = self.pop_root()
        
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
        self.push_root(lp1)
        self.commit_transaction()
        lp1 = self.pop_root()
        
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
        self.push_root(lp1)
        self.commit_transaction()
        lp1 = self.pop_root()

        self.start_transaction()
        stm_set_char(lp1, 'x')
        self.abort_transaction()

        self.start_transaction()
        assert stm_get_char(lp1) == 'a'

    def test_inevitable_transaction_has_priority(self):
        self.start_transaction()
        lp1 = stm_allocate(16)
        stm_set_char(lp1, 'a')
        self.push_root(lp1)
        self.commit_transaction()
        lp1 = self.pop_root()

        self.start_transaction()
        stm_read(lp1)
        #
        self.switch(1)
        self.start_transaction()
        stm_write(lp1)
        stm_set_char(lp1, 'b')
        stm_become_inevitable()
        self.commit_transaction()
        #
        py.test.raises(Conflict, self.switch, 0)
        self.start_transaction()
        assert stm_get_char(lp1) == 'b'

    def test_object_on_two_pages(self):
        self.start_transaction()
        lp1 = stm_allocate(4104)
        stm_set_char(lp1, '0')
        stm_set_char(lp1, '1', offset=4103)
        self.commit_transaction()
        #
        self.start_transaction()
        stm_set_char(lp1, 'a')
        stm_set_char(lp1, 'b', offset=4103)
        #
        self.switch(1)
        self.start_transaction()
        assert stm_get_char(lp1) == '0'
        assert stm_get_char(lp1, offset=4103) == '1'
        self.commit_transaction()
        #
        self.switch(0)
        self.commit_transaction()
        #
        self.switch(1)
        self.start_transaction()
        assert stm_get_char(lp1) == 'a'
        assert stm_get_char(lp1, offset=4103) == 'b'
        self.commit_transaction()

    def test_abort_restores_shadowstack(self):
        self.start_transaction()
        self.push_root(ffi.cast("object_t *", 0))
        self.abort_transaction()
        py.test.raises(EmptyStack, self.pop_root)

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
