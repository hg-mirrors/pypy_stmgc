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

    def test_simple_write(self):
        stm_start_transaction()
        lp1, _  = stm_allocate(16)
        assert stm_was_written(lp1)
        stm_write(lp1)
        assert stm_was_written(lp1)

    def test_allocate_old(self):
        lp1, _ = stm_allocate_old(16)
        self.switch(1)
        lp2, _ = stm_allocate_old(16)
        assert lp1 != lp2
        
    def test_write_on_old(self):
        lp1, p1 = stm_allocate_old(16)
        stm_start_transaction()
        stm_write(lp1)
        p1[15] = 'a'
        self.switch(1)
        stm_start_transaction()
        stm_read(lp1)
        tp1 = stm_get_real_address(lp1)
        assert tp1[15] == '\0'
        
        

    def test_read_write_1(self):
        stm_start_transaction()
        p1 = stm_allocate(16)
        p1[8] = 'a'
        stm_stop_transaction(False)
        #
        self.switch("sub1")
        stm_start_transaction()
        stm_write(p1)
        assert p1[8] == 'a'
        p1[8] = 'b'
        #
        self.switch("main")
        stm_start_transaction()
        stm_read(p1)
        assert p1[8] == 'a'
        #
        self.switch("sub1")
        stm_stop_transaction(False)
        #
        self.switch("main")
        assert p1[8] == 'a'

    def test_start_transaction_updates(self):
        stm_start_transaction()
        p1 = stm_allocate(16)
        p1[8] = 'a'
        stm_stop_transaction(False)
        #
        self.switch("sub1")
        stm_start_transaction()
        stm_write(p1)
        assert p1[8] == 'a'
        p1[8] = 'b'
        stm_stop_transaction(False)
        #
        self.switch("main")
        assert p1[8] == 'a'
        stm_start_transaction()
        assert p1[8] == 'b'

    def test_resolve_no_conflict_empty(self):
        stm_start_transaction()
        #
        self.switch("sub1")
        stm_start_transaction()
        stm_stop_transaction(False)
        #
        self.switch("main")
        stm_stop_transaction(False)

    def test_resolve_no_conflict_write_only_in_already_committed(self):
        stm_start_transaction()
        p1 = stm_allocate(16)
        p1[8] = 'a'
        stm_stop_transaction(False)
        stm_start_transaction()
        #
        self.switch("sub1")
        stm_start_transaction()
        stm_write(p1)
        p1[8] = 'b'
        stm_stop_transaction(False)
        #
        self.switch("main")
        assert p1[8] == 'a'
        stm_stop_transaction(False)
        assert p1[8] == 'b'

    def test_resolve_write_read_conflict(self):
        stm_start_transaction()
        p1 = stm_allocate(16)
        p1[8] = 'a'
        stm_stop_transaction(False)
        stm_start_transaction()
        #
        self.switch("sub1")
        stm_start_transaction()
        stm_write(p1)
        p1[8] = 'b'
        stm_stop_transaction(False)
        #
        self.switch("main")
        stm_read(p1)
        assert p1[8] == 'a'
        stm_stop_transaction(expected_conflict=True)
        assert p1[8] in ('a', 'b')
        stm_start_transaction()
        assert p1[8] == 'b'

    def test_resolve_write_write_conflict(self):
        stm_start_transaction()
        p1 = stm_allocate(16)
        p1[8] = 'a'
        stm_stop_transaction(False)
        stm_start_transaction()
        #
        self.switch("sub1")
        stm_start_transaction()
        stm_write(p1)
        p1[8] = 'b'
        stm_stop_transaction(False)
        #
        self.switch("main")
        assert p1[8] == 'a'
        stm_write(p1)
        p1[8] = 'c'
        stm_stop_transaction(expected_conflict=True)
        assert p1[8] in ('a', 'b')
        stm_start_transaction()
        assert p1[8] == 'b'

    def test_resolve_write_write_no_conflict(self):
        stm_start_transaction()
        p1 = stm_allocate(16)
        p2 = stm_allocate(16)
        p1[8] = 'a'
        p2[8] = 'A'
        stm_stop_transaction(False)
        stm_start_transaction()
        #
        self.switch("sub1")
        stm_start_transaction()
        stm_write(p1)
        p1[8] = 'b'
        stm_stop_transaction(False)
        #
        self.switch("main")
        stm_write(p2)
        p2[8] = 'C'
        stm_stop_transaction(False)
        assert p1[8] == 'b'
        assert p2[8] == 'C'

    def test_page_extra_malloc_unchanged_page(self):
        stm_start_transaction()
        p1 = stm_allocate(16)
        p2 = stm_allocate(16)
        p1[8] = 'A'
        p2[8] = 'a'
        stm_stop_transaction(False)
        stm_start_transaction()
        #
        self.switch("sub1")
        stm_start_transaction()
        stm_write(p1)
        assert p1[8] == 'A'
        p1[8] = 'B'
        stm_stop_transaction(False)
        #
        self.switch("main")
        stm_read(p2)
        assert p2[8] == 'a'
        p3 = stm_allocate(16)   # goes into the same page, which is
        p3[8] = ':'             #  not otherwise modified
        stm_stop_transaction(False)
        #
        assert p1[8] == 'B'
        assert p2[8] == 'a'
        assert p3[8] == ':'

    def test_page_extra_malloc_changed_page_before(self):
        stm_start_transaction()
        p1 = stm_allocate(16)
        p2 = stm_allocate(16)
        p1[8] = 'A'
        p2[8] = 'a'
        stm_stop_transaction(False)
        stm_start_transaction()
        #
        self.switch("sub1")
        stm_start_transaction()
        stm_write(p1)
        assert p1[8] == 'A'
        p1[8] = 'B'
        stm_stop_transaction(False)
        #
        self.switch("main")
        stm_write(p2)
        assert p2[8] == 'a'
        p2[8] = 'b'
        p3 = stm_allocate(16)  # goes into the same page, which I already
        p3[8] = ':'            #  modified just above
        stm_stop_transaction(False)
        #
        assert p1[8] == 'B'
        assert p2[8] == 'b'
        assert p3[8] == ':'

    def test_page_extra_malloc_changed_page_after(self):
        stm_start_transaction()
        p1 = stm_allocate(16)
        p2 = stm_allocate(16)
        p1[8] = 'A'
        p2[8] = 'a'
        stm_stop_transaction(False)
        stm_start_transaction()
        #
        self.switch("sub1")
        stm_start_transaction()
        stm_write(p1)
        assert p1[8] == 'A'
        p1[8] = 'B'
        stm_stop_transaction(False)
        #
        self.switch("main")
        p3 = stm_allocate(16)  # goes into the same page, which I will
        p3[8] = ':'            #  modify just below
        stm_write(p2)
        assert p2[8] == 'a'
        p2[8] = 'b'
        stm_stop_transaction(False)
        #
        assert p1[8] == 'B'
        assert p2[8] == 'b'
        assert p3[8] == ':'

    def test_overflow_write_history(self):
        stm_start_transaction()
        plist = [stm_allocate(n) for n in range(16, 256, 8)]
        stm_stop_transaction(False)
        #
        for i in range(20):
            stm_start_transaction()
            for p in plist:
                stm_write(p)
            stm_stop_transaction(False)
