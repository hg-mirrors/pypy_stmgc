from support import *


class TestBasic(BaseTest):

    def test_thread_local_allocations(self):
        p1 = stm_allocate(16)
        p2 = stm_allocate(16)
        assert intptr(p2) - intptr(p1) == 16
        p3 = stm_allocate(16)
        assert intptr(p3) - intptr(p2) == 16
        #
        self.switch("sub1")
        p1s = stm_allocate(16)
        assert abs(intptr(p1s) - intptr(p3)) >= 4000
        #
        self.switch("main")
        p4 = stm_allocate(16)
        assert intptr(p4) - intptr(p3) == 16

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
