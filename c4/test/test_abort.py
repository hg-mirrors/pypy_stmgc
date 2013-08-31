import py
from support import *

# -------------------------------------------------------------------------
# General warning: this tests aborts by really running a C setjmp(longjmp.
# This kind of works if there are Python frames in-between, as long as
# we fix a few things (see _GC_ON_CPYTHON), but not out-of-the-box on PyPy.
# -------------------------------------------------------------------------


def setup_function(f):
    lib.stm_clear_between_tests()
    lib.stm_initialize_tests(getattr(f, 'max_aborts', 0))

def teardown_function(_):
    lib.stm_finalize()


def test_peform_transaction():
    seen = []
    #
    @perform_transaction
    def run(retry_counter):
        if len(seen) < 5:
            seen.append(retry_counter)
            return True
    assert seen == [0] * 5

def test_abort():
    seen = []
    #
    @perform_transaction
    def run(retry_counter):
        if len(seen) < 5000:
            seen.append(retry_counter)
            abort_and_retry()
    assert seen == range(5000)

def test_global_to_local_copies():
    p1 = palloc(HDR + WORD)
    #
    @perform_transaction
    def run(retry_counter):
        p2 = lib.stm_write_barrier(p1)
        if retry_counter == 0:
            abort_and_retry()
        else:
            major_collect()

def test_old_objects_to_trace():
    py.test.skip("we shouldn't trace objects from an aborted "
                 "transaction but well")
    p1 = palloc_refs(1)
    #
    @perform_transaction
    def run(retry_counter):
        if retry_counter == 0:
            p2 = lib.stm_write_barrier(p1)
            lib.stm_push_root(p2)
            minor_collect()
            p2 = lib.stm_pop_root()
            setptr(p2, 0, nalloc(HDR))
            assert p2 == lib.stm_write_barrier(p2)
            # write some invalid pointer, we're aborting anyway
            lib.rawsetlong(p2, 0, 0x404)
            abort_and_retry()
        else:
            major_collect()

def test_push_restore():
    @perform_transaction
    def run(retry_counter):
        if retry_counter == 0:
            lib.stm_push_root(ffi.NULL)
            abort_and_retry()

def test_young_objects_outside_nursery():
    py.test.skip("we shouldn't trace objects from an aborted "
                 "transaction but well")
    p1list = [palloc_refs(1) for i in range(20)]
    #
    @perform_transaction
    def run(retry_counter):
        if retry_counter == 0:
            # make sure there are a couple of young_objects_outside_nursery
            p2list = [lib.stm_write_barrier(p1) for p1 in p1list]
            for p2 in p2list:
                lib.stm_push_root(p2)
            minor_collect()
            # write some invalid pointers, we're aborting anyway
            for i in range(len(p2list)):
                p2 = lib.stm_pop_root()
                lib.rawsetlong(p2, 0, 0x404)
            abort_and_retry()
        else:
            major_collect()

def test_abort_protected_to_private():
    pg = palloc(HDR + WORD)
    p1 = lib.stm_write_barrier(pg)
    lib.setlong(p1, 0, 32128247)
    #
    @perform_transaction
    def run(retry_counter):
        assert lib.getlong(pg, 0) == 32128247
        if retry_counter == 0:
            lib.setlong(pg, 0, 4737372)
            abort_and_retry()
        else:
            lib.setlong(pg, 0, 838311)
    #
    assert lib.getlong(pg, 0) == 838311
