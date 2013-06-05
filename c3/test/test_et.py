import py
from support import *


def setup_function(f):
    lib.stm_clear_between_tests()
    lib.stm_initialize_tests(getattr(f, 'max_aborts', 0))

def teardown_function(_):
    lib.stm_finalize()


def test_freshly_created():
    p = nalloc(HDR)
    r = lib.get_private_rev_num()
    assert r < 0 and r % 2 == 1
    assert p.h_revision == r
    assert p.h_tid == lib.gettid(p) | 0    # no GC flags

def test_write_barrier_private():
    p = nalloc(HDR)
    assert lib.stm_write_barrier(p) == p
    assert p.h_revision == lib.get_private_rev_num()
    assert p.h_tid == lib.gettid(p) | 0    # no GC flags

def test_protected_no_backup():
    p = nalloc(HDR)
    r = lib.get_private_rev_num()
    assert p.h_revision == r
    lib.stm_commit_transaction()
    lib.stm_begin_inevitable_transaction()
    r2 = lib.get_private_rev_num()
    assert r2 < 0 and r2 % 2 == 1
    assert r != r2
    assert p.h_revision == r
    assert p.h_tid == lib.gettid(p) | 0    # no GC flags

def test_private_with_backup():
    p = nalloc(HDR)
    lib.stm_commit_transaction()
    lib.stm_begin_inevitable_transaction()
    r2 = lib.get_private_rev_num()
    assert p.h_revision != r2
    p2 = lib.stm_write_barrier(p)
    assert p2 == p       # does not move
    assert p.h_revision == r2

def test_get_backup_copy():
    p = nalloc(HDR + WORD)
    lib.setlong(p, 0, 78927812)
    lib.stm_commit_transaction()
    lib.stm_begin_inevitable_transaction()
    org_r = p.h_revision
    lib.setlong(p, 0, 927122)
    assert p.h_revision == lib.get_private_rev_num()
    pback = lib.stm_get_backup_copy(p)
    assert pback and pback != p
    assert pback.h_revision == org_r
    assert pback.h_tid == p.h_tid | GCFLAG_BACKUP_COPY
    assert lib.rawgetlong(pback, 0) == 78927812
    assert lib.rawgetlong(p, 0) == 927122

def test_protected_with_backup():
    xxx
