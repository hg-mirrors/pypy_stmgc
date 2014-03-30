from support import *
import py, time


class TestTiming(BaseTest):

    def gettimer(self, n):
        tl = self.tls[self.current_thread]
        lib.stm_flush_timing(tl, 1)
        return tl.events[n], tl.timing[n]

    def expect_timer(self, n, expected_time, expected_count='?'):
        count, real = self.gettimer(n)
        print 'timer %d is %d;%s, expecting %s;%s' % (n, count, real,
            expected_count, expected_time)
        if expected_time == 0.0:
            assert real == 0.0
        elif expected_time == "nonzero":
            assert real > 0.0
        else:
            assert abs(real - expected_time) < 0.09
        if expected_count != '?':
            assert count == expected_count

    def test_time_outside_transaction(self):
        time.sleep(0.2)
        self.start_transaction()
        self.commit_transaction()
        self.expect_timer(lib.STM_TIME_OUTSIDE_TRANSACTION, 0.2)

    def test_time_run_current(self):
        self.start_transaction()
        time.sleep(0.1)
        self.expect_timer(lib.STM_TIME_RUN_CURRENT, 0.1, 0)
        time.sleep(0.1)
        self.expect_timer(lib.STM_TIME_RUN_CURRENT, 0.2, 0)
        self.commit_transaction()
        self.expect_timer(lib.STM_TIME_RUN_CURRENT, 0.0, 1)

    def test_time_run_committed(self):
        self.start_transaction()
        time.sleep(0.2)
        self.expect_timer(lib.STM_TIME_RUN_COMMITTED, 0.0, 0)
        self.commit_transaction()
        self.expect_timer(lib.STM_TIME_RUN_COMMITTED, 0.2, 1)

    def test_time_run_aborted_write_write(self):
        o = stm_allocate_old(16)
        self.start_transaction()
        stm_write(o)
        #
        self.switch(1)
        self.start_transaction()
        time.sleep(0.2)
        py.test.raises(Conflict, stm_write, o)
        self.expect_timer(lib.STM_TIME_RUN_ABORTED_WRITE_WRITE, 0.2, 1)

    def test_time_run_aborted_write_read(self):
        o = stm_allocate_old(16)
        self.start_transaction()
        stm_read(o)
        #
        self.switch(1)
        self.start_transaction()
        time.sleep(0.2)
        stm_write(o)
        py.test.raises(Conflict, self.commit_transaction)
        self.expect_timer(lib.STM_TIME_RUN_ABORTED_WRITE_READ, 0.2, 1)

    def test_time_run_aborted_inevitable(self):
        self.start_transaction()
        self.become_inevitable()
        #
        self.switch(1)
        self.start_transaction()
        time.sleep(0.2)
        py.test.raises(Conflict, self.become_inevitable)
        self.expect_timer(lib.STM_TIME_RUN_ABORTED_INEVITABLE, 0.2, 1)

    def test_time_run_aborted_other(self):
        self.start_transaction()
        time.sleep(0.2)
        self.abort_transaction()
        self.expect_timer(lib.STM_TIME_RUN_ABORTED_OTHER, 0.2, 1)

    def test_time_minor_gc(self):
        self.start_transaction()
        self.expect_timer(lib.STM_TIME_MINOR_GC, 0.0, 0)
        stm_minor_collect()
        self.expect_timer(lib.STM_TIME_MINOR_GC, "nonzero", 1)
        self.expect_timer(lib.STM_TIME_MAJOR_GC, 0.0, 0)

    def test_time_major_gc(self):
        self.start_transaction()
        self.expect_timer(lib.STM_TIME_MAJOR_GC, 0.0, 0)
        stm_major_collect()
        self.expect_timer(lib.STM_TIME_MAJOR_GC, "nonzero", 1)
