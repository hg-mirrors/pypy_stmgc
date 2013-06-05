import py
from support import *


def setup_function(f):
    lib.stm_clear_between_tests()
    lib.stm_initialize_tests(getattr(f, 'max_aborts', 0))

def teardown_function(_):
    lib.stm_commit_transaction()
    lib.stm_begin_inevitable_transaction()
    lib.stm_finalize()


def test_freshly_created():
    p = nalloc(HDR)
    r = lib.get_private_rev_num()
    assert r < 0 and r % 2 == 1
    assert p.h_revision == r
    assert p.h_tid == lib.gettid(p) | 0    # no GC flags
    assert classify(p) == "private"

def test_write_barrier_private():
    p = nalloc(HDR)
    assert lib.stm_write_barrier(p) == p
    assert p.h_revision == lib.get_private_rev_num()
    assert p.h_tid == lib.gettid(p) | 0    # no GC flags
    assert classify(p) == "private"

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
    assert classify(p) == "protected"

def test_private_with_backup():
    p = nalloc(HDR)
    lib.stm_commit_transaction()
    lib.stm_begin_inevitable_transaction()
    r2 = lib.get_private_rev_num()
    assert p.h_revision != r2
    assert classify(p) == "protected"
    p2 = lib.stm_write_barrier(p)
    assert p2 == p       # does not move
    assert p.h_revision == r2
    assert classify(p) == "private"

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
    assert classify(p) == "private"
    assert classify(pback) == "backup"

def test_protected_with_backup():
    p = nalloc(HDR + WORD)
    lib.setlong(p, 0, 78927812)
    lib.stm_commit_transaction()
    lib.stm_begin_inevitable_transaction()
    lib.setlong(p, 0, 927122)
    pback = lib.stm_get_backup_copy(p)
    assert pback != p
    assert p.h_revision == lib.get_private_rev_num()
    lib.stm_commit_transaction()
    lib.stm_begin_inevitable_transaction()
    assert lib.stm_get_backup_copy(p) == ffi.NULL
    assert classify(p) == "protected"
    assert classify(pback) == "backup"
    assert ffi.cast("revision_t *", p.h_revision) == pback

def test_protected_backup_reused():
    p = nalloc(HDR + WORD)
    lib.setlong(p, 0, 78927812)
    lib.stm_commit_transaction()
    lib.stm_begin_inevitable_transaction()
    lib.setlong(p, 0, 927122)
    pback = lib.stm_get_backup_copy(p)
    assert pback != p
    lib.stm_commit_transaction()
    lib.stm_begin_inevitable_transaction()
    assert lib.stm_get_backup_copy(p) == ffi.NULL
    assert classify(p) == "protected"
    assert classify(pback) == "backup"
    assert lib.rawgetlong(p, 0) == 927122
    assert lib.rawgetlong(pback, 0) == 78927812    # but should not be used
    lib.setlong(p, 0, 43891)
    assert p.h_revision == lib.get_private_rev_num()
    assert pback == lib.stm_get_backup_copy(p)
    assert lib.rawgetlong(p, 0) == 43891
    assert lib.rawgetlong(pback, 0) == 927122

def test_prebuilt_is_public():
    p = palloc(HDR)
    assert p.h_revision == 1
    assert p.h_tid == lib.gettid(p) | (GCFLAG_OLD |
                                       GCFLAG_VISITED |
                                       GCFLAG_PUBLIC |
                                       GCFLAG_PREBUILT_ORIGINAL)
    assert classify(p) == "public"

def test_change_prebuilt_object():
    p = palloc(HDR + WORD)
    lib.rawsetlong(p, 0, 28971289)
    flags = p.h_tid
    assert (flags & GCFLAG_PUBLIC_TO_PRIVATE) == 0
    assert classify(p) == "public"
    p2 = lib.stm_write_barrier(p)
    assert p2 != p
    assert classify(p) == "public"
    assert classify(p2) == "private"
    assert p.h_tid == flags | GCFLAG_PUBLIC_TO_PRIVATE
