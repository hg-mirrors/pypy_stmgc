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
    plist = [palloc(HDR) for i in range(6)]
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
