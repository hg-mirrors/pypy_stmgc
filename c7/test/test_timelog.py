import py, time
from support import *


class TestTimeLog(BaseTest):

    def test_empty(self):
        self.start_transaction()
        tlog = self.fetch_and_remove_timelog()
        assert tlog == ffi.NULL

    def test_simple_abort(self):
        self.start_transaction()
        start = time.time()
        while abs(time.time() - start) <= 0.05:
            pass
        self.abort_transaction()
        #
        self.start_transaction()
        tlog = self.fetch_and_remove_timelog()
        assert tlog != ffi.NULL
        assert tlog.reason == lib.STM_LOG_REASON_UNKNOWN
        assert tlog.contention == lib.STM_LOG_CONTENTION_NONE
        assert tlog.user == 0
        assert 0.0499 <= tlog.time_lost < 1.0
        lib.stm_free_timelog(tlog)
