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
        assert last_commit_log_entries() == []

        self.switch(0)

        self.commit_transaction()
        assert last_commit_log_entries() == []

    def test_simple_read(self):
        self.start_transaction()
        lp1 = stm_allocate(16)
        stm_read(lp1)
        assert stm_was_read(lp1)
        self.commit_transaction()
        assert last_commit_log_entries() == []

    def test_simple_write(self):
        self.start_transaction()
        lp1  = stm_allocate(16)
        assert stm_was_written(lp1)
        stm_write(lp1)
        assert stm_was_written(lp1)
        assert modified_old_objects() == []             # object not old
        assert objects_pointing_to_nursery() == []    # short transaction
        self.commit_transaction()
        assert last_commit_log_entries() == []

    def test_allocate_old(self):
        lp1 = stm_allocate_old(16)
        self.switch(1) # actually has not much of an effect...
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
        #
        self.start_transaction()
        self.commit_transaction()
        #
        self.switch(1)
        self.start_transaction()
        assert modified_old_objects() == []
        stm_write(lp1)
        assert modified_old_objects() == [lp1]
        assert objects_pointing_to_nursery() == [lp1]
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
        assert last_commit_log_entries() == [lp1]
        #
        py.test.raises(Conflict, self.switch, 0) # detects rw conflict

    @py.test.mark.parametrize("only_bk", [0, 1])
    def test_read_write_11(self, only_bk):
        # test that stm_validate() and the SEGV-handler
        # always ensure up-to-date views of pages:
        lp1 = stm_allocate_old(16)
        stm_get_real_address(lp1)[HDR] = 'a' #setchar
        #
        self.start_transaction()
        stm_set_char(lp1, '0') # shared->private
        # prot_none in seg: 1,2,3
        #
        self.switch(1)
        self.start_transaction()
        stm_set_char(lp1, '1')
        # prot_none in seg: 2,3
        #
        self.switch(0)
        self.commit_transaction()
        assert last_commit_log_entries() == [lp1] # commit '0'
        #
        py.test.raises(Conflict, self.switch, 1)
        self.start_transaction() # updates to '0'
        stm_set_char(lp1, 'x')
        self.commit_transaction()
        assert last_commit_log_entries() == [lp1] # commit 'x'
        #
        if only_bk:
            self.start_transaction()
            stm_set_char(lp1, 'y') # 'x' is only in bk_copy
        #
        #
        self.switch(2)
        self.start_transaction() # stm_validate()
        res = stm_get_char(lp1) # should be 'x'
        self.commit_transaction()
        assert res == 'x'
        # if fails, segfault-handler copied from seg0 which
        # is out-of-date because seg1 committed 'x'
        # (seg1 hasn't done stm_validate() since)

    @py.test.mark.parametrize("only_bk", [0, 1])
    def test_read_write_12(self, only_bk):
        # test that stm_validate() and the SEGV-handler
        # always ensure up-to-date views of pages:
        lp1 = stm_allocate_old(16)
        stm_get_real_address(lp1)[HDR] = 'a' #setchar
        #
        self.switch(1)
        self.start_transaction()
        stm_set_char(lp1, '1') # shared->private
        # prot_none in seg: 0,2,3
        #
        self.switch(0)
        self.start_transaction()
        stm_set_char(lp1, '0')
        # prot_none in seg: 2,3
        #
        self.switch(1)
        self.commit_transaction()
        assert last_commit_log_entries() == [lp1]
        # '1' is committed
        #
        if only_bk:
            self.start_transaction()
            stm_set_char(lp1, 'y') # '1' is only in bk_copy
        #
        self.switch(2)
        self.start_transaction() # stm_validate()
        res = stm_get_char(lp1) # should be '1'
        self.commit_transaction()
        assert res == '1'
        # if res=='a', then we got the outdated page-view
        # of segment 0 that didn't do stm_validate() and
        # therefore is still outdated.
        py.test.raises(Conflict, self.switch, 0)

    @py.test.mark.parametrize("only_bk", [0, 1])
    def test_read_write_13(self, only_bk):
        # test that stm_validate() and the SEGV-handler
        # always ensure up-to-date views of pages:
        lp1 = stm_allocate_old(16)
        stm_get_real_address(lp1)[HDR] = 'a' #setchar
        #
        self.start_transaction()
        stm_set_char(lp1, '0') # shared->private
        # prot_none in seg: 1,2,3
        #
        self.switch(1)
        self.start_transaction()
        stm_set_char(lp1, '1')
        self.switch(2)
        self.start_transaction()
        # prot_none in seg: 2,3
        #
        self.switch(0)
        self.commit_transaction()
        assert last_commit_log_entries() == [lp1] # commit '0'
        #
        py.test.raises(Conflict, self.switch, 1)
        self.start_transaction() # updates to '0'
        stm_set_char(lp1, 'x')
        self.commit_transaction()
        assert last_commit_log_entries() == [lp1] # commit 'x'
        #
        if only_bk:
            self.start_transaction()
            stm_set_char(lp1, 'y') # 'x' is only in bk_copy
        #
        #
        self.switch(2, validate=False) # NO stm_validate
        res = stm_get_char(lp1) # SEGV -> should not validate and go back in time -> 'a'
        py.test.raises(Conflict, self.commit_transaction) # 'a' is outdated, fail to commit
        assert res == 'a'

    @py.test.mark.parametrize("only_bk", [0, 1])
    def test_read_write_14(self, only_bk):
        lp1 = stm_allocate_old(16) # allocated in seg0
        stm_get_real_address(lp1)[HDR] = 'a' #setchar
        # S|P|P|P|P
        #
        # NO_ACCESS in all segments except seg0 (shared page holder)
        #
        #
        self.switch(2)
        self.start_transaction() # stm_validate()
        #
        self.switch(1) # with private page
        self.start_transaction()
        stm_set_char(lp1, 'C')
        self.commit_transaction()
        assert last_commit_log_entries() == [lp1] # commit 'C'
        #
        if only_bk:
            self.start_transaction()
            stm_set_char(lp1, 'y') # '1' is only in bk_copy
        #
        #
        self.switch(2, validate=False)
        res = stm_get_char(lp1) # should be 'a'
        py.test.raises(Conflict, self.commit_transaction)
        assert res == 'a'

    @py.test.mark.parametrize("only_bk", [0, 1])
    def test_read_write_15(self, only_bk):
        lp1 = stm_allocate_old(16) # allocated in seg0
        lp2 = stm_allocate_old(16) # allocated in seg0
        stm_get_real_address(lp1)[HDR] = 'a' #setchar
        stm_get_real_address(lp2)[HDR] = 'b' #setchar
        # S|P|P|P|P
        #
        # NO_ACCESS in all segments except seg0 (shared page holder)
        #
        # all seg at R0
        #
        self.start_transaction()
        #
        self.switch(1) # with private page
        self.start_transaction()
        stm_set_char(lp2, 'C')
        self.commit_transaction() # R1
        assert last_commit_log_entries() == [lp2] # commit 'C'
        if only_bk:
            self.start_transaction()
            stm_set_char(lp2, 'c') # R1.1
        #
        self.switch(2)
        self.start_transaction()
        stm_set_char(lp1, 'D')
        self.commit_transaction()  # R2
        assert last_commit_log_entries() == [lp1] # commit 'D'
        if only_bk:
            self.start_transaction()
            stm_set_char(lp1, 'd') # R2.1
        #
        self.switch(3)
        self.start_transaction() # stm_validate() -> R2
        assert stm_get_char(lp1) == 'D'
        assert stm_get_char(lp2) == 'C'
        self.commit_transaction()
        #


    def test_read_write_16(self):
        lp1 = stm_allocate_old(16) # allocated in seg0
        lp2 = stm_allocate_old(16) # allocated in seg0
        stm_get_real_address(lp1)[HDR] = 'a' #setchar
        stm_get_real_address(lp2)[HDR] = 'b' #setchar
        # S|P|P|P|P
        #
        # NO_ACCESS in all segments except seg0 (shared page holder)
        #
        # all seg at R0
        #
        self.start_transaction()
        #
        self.switch(1) # with private page
        self.start_transaction()
        stm_set_char(lp2, 'C')
        self.commit_transaction() # R1
        assert last_commit_log_entries() == [lp2] # commit 'C'
        #
        self.switch(2)
        self.start_transaction()
        stm_set_char(lp1, 'D')
        self.commit_transaction()  # R2
        assert last_commit_log_entries() == [lp1] # commit 'D'
        #
        self.switch(3)
        self.start_transaction() # stm_validate() -> R2
        assert stm_get_char(lp1) == 'D' # R2
        #
        self.switch(2)
        self.start_transaction()
        stm_set_char(lp1, 'I')
        self.commit_transaction()  # R2
        assert last_commit_log_entries() == [lp1] # commit 'I'
        #
        self.switch(1)
        self.start_transaction()
        stm_set_char(lp2, 'H')
        self.commit_transaction() # R3
        assert last_commit_log_entries() == [lp2] # commit 'H'
        #
        self.switch(3, validate=False) # R2 again
        assert stm_get_char(lp1) == 'D' # R2
        assert stm_get_char(lp2) == 'C' # R2
        py.test.raises(Conflict, self.commit_transaction)
        #



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
        assert last_commit_log_entries() == [lp]

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
        self.commit_transaction()

        py.test.raises(Conflict, self.switch, 0) # fails validation

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
        stm_write(lp1)
        self.switch(0)
        self.commit_transaction()

        py.test.raises(Conflict, self.switch, 1) # write-write conflict

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
        assert lib.stm_is_inevitable() == 0
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
        assert lib.stm_is_inevitable() == 0
        self.become_inevitable()
        assert lib.stm_is_inevitable() == 1
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
        self.push_root(lp1)
        self.commit_transaction()
        lp1 = self.pop_root()
        self.push_root(lp1)
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

    def test_abort_restores_shadowstack_inv(self):
        py.test.skip("the logic to save/restore the shadowstack doesn't "
                     "work in these tests")
        self.push_root(ffi.cast("object_t *", 1234))
        self.start_transaction()
        p = self.pop_root()
        assert p == ffi.cast("object_t *", 1234)
        self.push_root(ffi.cast("object_t *", 5678))
        self.pop_root()
        self.abort_transaction()
        p = self.pop_root()
        assert p == ffi.cast("object_t *", 1234)

    def test_check_content_after_commit(self):
        self.start_transaction()
        lp1 = stm_allocate(16)
        stm_set_char(lp1, 'X')
        self.push_root(lp1)
        self.commit_transaction()
        lp1 = self.pop_root()
        self.check_char_everywhere(lp1, 'X')

    def test_last_abort__bytes_in_nursery(self):
        self.start_transaction()
        stm_allocate(56)
        self.abort_transaction()
        assert self.get_stm_thread_local().last_abort__bytes_in_nursery == 56
        self.start_transaction()
        assert self.get_stm_thread_local().last_abort__bytes_in_nursery == 56
        self.commit_transaction()
        assert self.get_stm_thread_local().last_abort__bytes_in_nursery == 56
        self.start_transaction()
        assert self.get_stm_thread_local().last_abort__bytes_in_nursery == 56
        self.abort_transaction()
        assert self.get_stm_thread_local().last_abort__bytes_in_nursery == 0

    def test_abort_in_segfault_handler(self):
        lp1 = stm_allocate_old(16)
        lp2 = stm_allocate_old(16)

        self.start_transaction()
        stm_set_char(lp1, 'A')
        stm_set_char(lp2, 'B')
        self.commit_transaction()
        # lp1 = S|N|N|N
        # lp2 = S|N|N|N

        self.switch(1)

        self.start_transaction()
        assert stm_get_char(lp1) == 'A'
        # lp1 = S|P|N|N
        # lp2 = S|N|N|N

        self.switch(0)

        self.start_transaction()
        stm_set_char(lp1, 'C')
        self.commit_transaction()
        # lp1 = S|P|N|N
        # lp2 = S|N|N|N

        self.switch(1, validate=False)

        # seg1 segfaults, validates, and aborts:
        py.test.raises(Conflict, stm_get_char, lp2)
