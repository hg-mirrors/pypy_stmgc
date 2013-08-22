import py
from support import *


def setup_function(f):
    lib.stm_clear_between_tests()
    lib.stm_initialize_tests(getattr(f, 'max_aborts', 0))

def teardown_function(_):
    lib.stm_finalize()


def test_should_break_transaction():
    # we're inevitable here, so:
    assert should_break_transaction() == True
    #
    @perform_transaction
    def run(retry_counter):
        assert should_break_transaction() == False

def test_set_transaction_length():
    lib.stm_set_transaction_length(5)    # breaks after 4 read-or-writes
    plist = [palloc(HDR + WORD) for i in range(6)]
    should_br = ['?'] * (len(plist) + 1)
    #
    @perform_transaction
    def run(retry_counter):
        should_br[0] = should_break_transaction()
        for i in range(len(plist)):
            lib.stm_write_barrier(plist[i])
            should_br[i + 1] = should_break_transaction()
    #
    assert should_br == [False, False, False, False, True, True, True]

def test_stm_atomic():
    assert lib.stm_atomic(0) == 0
    x = lib.stm_atomic(+1)
    assert x == 1
    x = lib.stm_atomic(+1)
    assert x == 2
    x = lib.stm_atomic(-1)
    assert x == 1
    x = lib.stm_atomic(0)
    assert x == 1
    x = lib.stm_atomic(-1)
    assert x == 0

def test_transaction_atomic_mode():
    assert lib.stm_in_transaction()
    lib.stm_commit_transaction()
    assert not lib.stm_in_transaction()
    lib.stm_begin_inevitable_transaction()
    assert lib.stm_in_transaction()
    lib.stm_atomic(+1)
    lib.stm_commit_transaction()
    assert lib.stm_in_transaction()
    lib.stm_begin_inevitable_transaction()
    lib.stm_atomic(-1)

def test_atomic_but_abort():
    @perform_transaction
    def run(retry_counter):
        assert lib.stm_atomic(0) == 0
        if retry_counter == 0:
            lib.stm_atomic(+1)
            abort_and_retry()

def test_entering_atomic():
    seen = []
    def run1(c1):
        assert c1 == 0
        lib.stm_atomic(+1)
        def run2(c2):
            if c2 == 0:
                if not seen:
                    assert lib.stm_atomic(0) == 1
                    lib.stm_atomic(-1)
                    seen.append("continue running, but now in non-atomic mode")
                    return True
                assert lib.stm_atomic(0) == 0
                seen.append("aborting now")
                abort_and_retry()
            seen.append("done!")
        perform_transaction(run2)
    perform_transaction(run1)
    assert len(seen) == len(set(seen)) == 3

def test_bug_v_atomic():
    p1 = palloc(HDR + WORD)
    #
    def f1(r):
        def cb(retry_counter):
            assert retry_counter == 0
            r.enter_in_parallel()
            lib.setlong(p1, 0, 1111)
            lib.stm_commit_transaction()
            lib.stm_begin_inevitable_transaction()
            r.leave_in_parallel()
        perform_transaction(cb)
    #
    def f2(r):
        def cb(retry_counter):
            if retry_counter == 0:
                lib.setlong(p1, 0, 2222)
                r.wait_while_in_parallel()
                # finish the transaction, but it will abort
                lib.stm_atomic(+1)
        perform_transaction(cb)
    #
    run_parallel(f1, f2, max_aborts=1)
