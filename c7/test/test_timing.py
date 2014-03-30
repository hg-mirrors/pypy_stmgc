from support import *
import py, time


class TestTiming(BaseTest):

    def gettimer(self, n):
        tl = self.tls[self.current_thread]
        lib.stm_flush_timing(tl)
        return tl.timing[n]

    def expect_timer(self, n, expected_value):
        real = self.gettimer(n)
        print 'timer %d is %s, expecting %s' % (n, real, expected_value)
        assert abs(real - expected_value) < 0.09

    def test_time_outside_transaction(self):
        time.sleep(0.2)
        self.start_transaction()
        self.commit_transaction()
        self.expect_timer(lib.STM_TIME_OUTSIDE_TRANSACTION, 0.2)

    def test_time_run_current(self):
        self.start_transaction()
        time.sleep(0.1)
        self.expect_timer(lib.STM_TIME_RUN_CURRENT, 0.1)
        time.sleep(0.1)
        self.expect_timer(lib.STM_TIME_RUN_CURRENT, 0.2)
        self.commit_transaction()
        self.expect_timer(lib.STM_TIME_RUN_CURRENT, 0.0)

    def test_time_run_committed(self):
        self.start_transaction()
        time.sleep(0.2)
        self.expect_timer(lib.STM_TIME_RUN_COMMITTED, 0.0)
        self.commit_transaction()
        self.expect_timer(lib.STM_TIME_RUN_COMMITTED, 0.2)
