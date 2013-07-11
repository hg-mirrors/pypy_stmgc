import py
from support import *


SHORTCUT = True


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
    assert lib.stm_id(p) != 0

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
    assert classify(p) == "private_from_protected"
    pback = follow_revision(p)
    assert classify(pback) == "backup"
    assert list_of_private_from_protected() == [p]

def test_get_backup_copy():
    p = nalloc(HDR + WORD)
    lib.setlong(p, 0, 78927812)
    lib.stm_commit_transaction()
    lib.stm_begin_inevitable_transaction()
    org_r = p.h_revision
    assert classify(p) == "protected"
    lib.setlong(p, 0, 927122)
    assert classify(p) == "private_from_protected"
    pback = follow_revision(p)
    assert pback and pback != p
    assert pback.h_revision == org_r
    assert pback.h_tid == ((p.h_tid & ~GCFLAG_PRIVATE_FROM_PROTECTED) |
                           GCFLAG_BACKUP_COPY | GCFLAG_OLD)
    assert lib.rawgetlong(pback, 0) == 78927812
    assert lib.rawgetlong(p, 0) == 927122
    assert classify(p) == "private_from_protected"
    assert classify(pback) == "backup"

def test_prebuilt_is_public():
    p = palloc(HDR)
    assert p.h_revision == 1
    assert p.h_tid == lib.gettid(p) | (GCFLAG_OLD |
                                       GCFLAG_VISITED |
                                       GCFLAG_PUBLIC |
                                       GCFLAG_PREBUILT_ORIGINAL)
    assert classify(p) == "public"
    assert lib.stm_id(p) != 0

def test_prebuilt_object_to_private():
    p = palloc(HDR)
    flags = p.h_tid
    assert (flags & GCFLAG_PUBLIC_TO_PRIVATE) == 0
    pid = lib.stm_id(p)
    assert classify(p) == "public"
    p2 = lib.stm_write_barrier(p)
    assert p2 != p
    assert classify(p) == "public"
    assert classify(p2) == "private"
    assert p.h_tid == flags | GCFLAG_PUBLIC_TO_PRIVATE
    assert pid == lib.stm_id(p2)

def test_commit_change_to_prebuilt_object():
    p = palloc(HDR + WORD)
    lib.rawsetlong(p, 0, 28971289)
    p2 = lib.stm_write_barrier(p)
    assert p2 != p
    assert classify(p) == "public"
    assert classify(p2) == "private"
    lib.rawsetlong(p2, 0, 1289222)
    lib.stm_commit_transaction()
    lib.stm_begin_inevitable_transaction()
    assert classify(p) == "public"
    assert classify(p2) == "protected"
    pstub = ffi.cast("gcptr", p.h_revision)
    assert classify(pstub) == "stub"
    assert stub_thread(pstub) == lib.my_stub_thread()
    assert lib.rawgetlong(p, 0) == 28971289
    assert lib.rawgetlong(p2, 0) == 1289222

def test_read_barrier_private():
    p = nalloc(HDR)
    assert lib.stm_read_barrier(p) == p     # no effect
    assert p.h_tid == gettid(p)
    assert p.h_revision == lib.get_private_rev_num()
    assert list_of_read_objects() == []

def test_read_barrier_protected():
    p = nalloc(HDR)
    lib.stm_commit_transaction()
    lib.stm_begin_inevitable_transaction()
    assert list_of_read_objects() == []
    assert lib.stm_read_barrier(p) == p     # record as a read object
    assert list_of_read_objects() == [p]

def test_read_barrier_public():
    p = palloc(HDR)
    assert lib.stm_read_barrier(p) == p
    assert list_of_read_objects() == [p]

def test_read_barrier_public_outdated():
    p1 = palloc(HDR)
    p2 = palloc(HDR)
    p1.h_revision = ffi.cast("revision_t", p2)
    assert lib.stm_read_barrier(p1) == p2
    assert list_of_read_objects() == [p2]

def test_read_barrier_public_shortcut():
    p1 = palloc(HDR)
    p2 = palloc(HDR)
    p3 = palloc(HDR)
    p1.h_revision = ffi.cast("revision_t", p2)
    p2.h_revision = ffi.cast("revision_t", p3)
    assert lib.stm_read_barrier(p1) == p3
    assert list_of_read_objects() == [p3]
    if not SHORTCUT:
        py.test.skip("re-enable!")
    assert p1.h_revision == int(ffi.cast("revision_t", p3))   # shortcutted

def test_read_barrier_public_to_private():
    p = palloc(HDR)
    pid = lib.stm_id(p)
    p2 = lib.stm_write_barrier(p)
    assert p2 != p
    assert classify(p) == "public"
    assert classify(p2) == "private"
    assert list_of_read_objects() == [p]
    assert p.h_tid & GCFLAG_PUBLIC
    assert p.h_tid & GCFLAG_PUBLIC_TO_PRIVATE
    p3 = lib.stm_read_barrier(p)
    assert p3 == p2
    assert list_of_read_objects() == [p]
    assert pid == lib.stm_id(p2)

def test_read_barrier_handle_protected():
    p = palloc(HDR)
    p2 = lib.stm_write_barrier(p)
    lib.stm_commit_transaction()
    lib.stm_begin_inevitable_transaction()
    assert classify(p) == "public"
    assert classify(p2) == "protected"
    assert list_of_read_objects() == []
    p3 = lib.stm_read_barrier(p)
    assert p3 == p2
    assert list_of_read_objects() == [p2]
    p4 = lib.stm_read_barrier(p)
    assert p4 == p2
    assert list_of_read_objects() == [p2]

def test_read_barrier_handle_private():
    p = palloc(HDR)
    p2 = lib.stm_write_barrier(p)
    lib.stm_commit_transaction()
    lib.stm_begin_inevitable_transaction()
    p2b = lib.stm_write_barrier(p)
    assert p2b == p2
    assert classify(p) == "public"
    assert classify(p2) == "private_from_protected"
    assert list_of_read_objects() == [p2]
    p3 = lib.stm_read_barrier(p)
    assert p3 == p2
    assert list_of_read_objects() == [p2]
    p4 = lib.stm_read_barrier(p)
    assert p4 == p2
    assert list_of_read_objects() == [p2]

def test_write_barrier_after_minor_collect():
    p = oalloc_refs(1)
    pw = lib.stm_write_barrier(p)

    lib.stm_push_root(pw)
    minor_collect()
    lib.stm_push_root(ffi.NULL)
    minor_collect()
    lib.stm_pop_root()
    minor_collect()
    r = nalloc(HDR)
    pw = lib.stm_pop_root()

    assert pw.h_tid & GCFLAG_OLD
    rawsetptr(pw, 0, r)

    # pw needs to be readded to old_objects_to_trace
    # before the next minor gc in order for this test to pass
    lib.stm_push_root(r)
    minor_collect()
    minor_collect()
    minor_collect()
    q = nalloc(HDR)
    r2 = lib.stm_pop_root()
    check_nursery_free(r)
    
    pr = lib.stm_read_barrier(p)
    assert r != r2
    assert getptr(pr, 0) != r
    assert getptr(pr, 0) == r2

    # the following shouldn't be done
    # because pw was not saved. Just
    # here to check that pw gets removed
    # from old_objects_to_trace when not found
    # on the root stack anymore
    rawsetptr(pw, 0, q)
    lib.stm_push_root(q)
    minor_collect()
    q2 = lib.stm_pop_root()
    check_nursery_free(q)
    pr = lib.stm_read_barrier(p)
    assert q != q2
    assert getptr(pr, 0) == q
    assert getptr(pr, 0) != q2


def test_id_young_to_old():
    # move out of nursery with shadow original
    p = nalloc(HDR)
    assert p.h_original == 0
    pid = lib.stm_id(p)
    assert p.h_tid & GCFLAG_HAS_ID
    porig = follow_original(p)
    assert porig.h_tid & GCFLAG_OLD
    lib.stm_push_root(p)
    minor_collect()
    p = lib.stm_pop_root()
    assert not lib.in_nursery(p)
    assert pid == lib.stm_id(p)

def test_id_private_from_protected():
    # read and write from protected
    p = oalloc(HDR)
    porig = follow_original(p)
    # impl detail {
    # old objects have id==itself, if not set differently
    assert porig == ffi.NULL
    # }

    p1 = oalloc(HDR)
    p1id = lib.stm_id(p1)
    p1r = lib.stm_read_barrier(p1)
    assert lib.stm_id(p1r) == p1id
    p1w = lib.stm_write_barrier(p1)
    assert lib.stm_id(p1w) == p1id

    p2 = oalloc(HDR)
    p2w = lib.stm_write_barrier(p2)
    p2id = lib.stm_id(p2w)
    assert p2id == lib.stm_id(p2)
    # impl detail {
    assert p2w.h_original == 0
    # }
    

def test_stealing_old():
    p = palloc(HDR + WORD)
    plist = [p]
    def f1(r):
        assert (p.h_tid & GCFLAG_PUBLIC_TO_PRIVATE) == 0
        p1 = lib.stm_write_barrier(p)   # private copy
        assert p1 != p
        assert classify(p) == "public"
        assert classify(p1) == "private"
        minor_collect()
        check_nursery_free(p1)
        p1 = lib.stm_read_barrier(p)
        assert p1.h_tid & GCFLAG_OLD
        assert p.h_tid & GCFLAG_PUBLIC_TO_PRIVATE
        lib.rawsetlong(p1, 0, 2782172)
        lib.stm_commit_transaction()
        lib.stm_begin_inevitable_transaction()
        p1id = lib.stm_id(p1)
        assert p1id == lib.stm_id(p)
        assert classify(p) == "public"
        assert classify(p1) == "protected"
        plist.append(p1)     # now p's most recent revision is protected
        assert classify(follow_revision(p)) == "stub"
        assert p1.h_revision & 1
        r.set(2)
        r.wait(3)
        assert classify(p1) == "public"
        assert lib.stm_read_barrier(p) == p1
        assert lib.stm_read_barrier(p1) == p1
        assert lib.stm_id(p1) == p1id
    def f2(r):
        r.wait(2)
        p2 = lib.stm_read_barrier(p)    # steals
        assert lib.stm_id(p) == lib.stm_id(p2)
        assert classify(p2) == "public"
        assert lib.rawgetlong(p2, 0) == 2782172
        assert p2 == lib.stm_read_barrier(p)    # short-circuit h_revision
        if SHORTCUT:
            assert p.h_revision == int(ffi.cast("revision_t", p2))
        assert p2 == lib.stm_read_barrier(p)
        assert p2 == plist[-1]
        assert classify(p2) == "public"
        r.set(3)
    run_parallel(f1, f2)

def test_stealing_young():
    p = palloc(HDR + WORD)
    plist = [p]
    def f1(r):
        assert (p.h_tid & GCFLAG_PUBLIC_TO_PRIVATE) == 0
        p1 = lib.stm_write_barrier(p)   # private copy
        assert p1 != p
        assert classify(p) == "public"
        assert classify(p1) == "private"
        assert p.h_tid & GCFLAG_PUBLIC_TO_PRIVATE
        lib.rawsetlong(p1, 0, 2782172)
        lib.stm_commit_transaction()
        lib.stm_begin_inevitable_transaction()
        assert classify(p) == "public"
        assert classify(p1) == "protected"
        plist.append(p1)     # now p's most recent revision is protected
        assert classify(follow_revision(p)) == "stub"
        assert p1.h_revision & 1
        p1id = lib.stm_id(p1)
        r.set(2)
        r.wait(3)
        assert p1id == lib.stm_id(p1)
        assert classify(p1) == "public"
        assert (p1.h_revision & 1) == 0    # outdated
        p2 = ffi.cast("gcptr", p1.h_revision)
        assert lib.in_nursery(p1)
        assert not lib.in_nursery(p2)
        assert lib.stm_read_barrier(p) == p2
        assert lib.stm_read_barrier(p1) == p2
        assert lib.stm_read_barrier(p2) == p2
    def f2(r):
        r.wait(2)
        p2 = lib.stm_read_barrier(p)    # steals
        assert lib.stm_id(p) == lib.stm_id(p2)
        assert classify(p2) == "public"
        assert lib.rawgetlong(p2, 0) == 2782172
        assert p2 == lib.stm_read_barrier(p)    # short-circuit h_revision
        if SHORTCUT:
            assert p.h_revision == int(ffi.cast("revision_t", p2))
        assert p2 == lib.stm_read_barrier(p)
        assert p2 != plist[-1]   # p2 is a public moved-out-of-nursery
        assert plist[-1].h_tid & GCFLAG_PUBLIC
        assert plist[-1].h_tid & GCFLAG_NURSERY_MOVED
        assert plist[-1].h_revision == int(ffi.cast("revision_t", p2))
        assert classify(p2) == "public"
        r.set(3)
    run_parallel(f1, f2)

def test_stealing_while_modifying(aborting=False):
    p = palloc(HDR + WORD)

    def f1(r):
        p1 = lib.stm_write_barrier(p)   # private copy
        assert classify(p) == "public"
        assert classify(p1) == "private"
        lib.rawsetlong(p1, 0, 2782172)
        minor_collect()
        check_nursery_free(p1)
        p1 = lib.stm_read_barrier(p)
        assert p1.h_tid & GCFLAG_OLD
        pback_ = []

        def cb(c):
            if c != 0:
                assert aborting
                [pback] = pback_
                assert classify(p) == "public"
                assert classify(p1) == "public"
                assert classify(pback) == "public"
                assert lib.stm_read_barrier(p) == pback
                assert lib.stm_read_barrier(p1) == pback
                return
            assert classify(p) == "public"
            assert classify(p1) == "protected"
            assert classify(follow_revision(p)) == "stub"
            p2 = lib.stm_write_barrier(p)
            assert p2 == p1
            lib.rawsetlong(p2, 0, -451112)
            pback = follow_revision(p1)
            pback_.append(pback)
            assert classify(p1) == "private_from_protected"
            assert classify(pback) == "backup"
            assert lib.stm_read_barrier(p) == p1
            assert lib.stm_read_barrier(p1) == p1
            assert pback.h_revision & 1
            r.wait_while_in_parallel()
            assert classify(p1) == "private_from_protected"
            assert classify(pback) == "public"
            assert pback.h_tid & GCFLAG_PUBLIC_TO_PRIVATE
            assert lib.stm_read_barrier(p) == p1
            assert lib.stm_read_barrier(p1) == p1
            assert lib.stm_read_barrier(pback) == p1
            assert pback.h_revision & 1
            if aborting:
                abort_and_retry()
        perform_transaction(cb)

        lib.stm_commit_transaction()
        lib.stm_begin_inevitable_transaction()
        [pback] = pback_
        if aborting:
            assert classify(p1) == "public"
            assert classify(pback) == "public"
            assert pback.h_revision & 1
            assert p1.h_revision == int(ffi.cast("revision_t", pback))
        else:
            assert classify(p1) == "protected"
            assert classify(pback) == "public"
            assert classify(follow_revision(pback)) == "stub"
            assert follow_revision(pback).h_revision == (
                int(ffi.cast("revision_t", p1)) | 2)

    def f2(r):
        def cb(c):
            assert c == 0
            r.enter_in_parallel()
            lib.stm_commit_transaction()
            lib.stm_begin_inevitable_transaction()
            p2 = lib.stm_read_barrier(p)    # steals
            assert lib.rawgetlong(p2, 0) == 2782172
            assert p2 == lib.stm_read_barrier(p)
            assert classify(p2) == "public"
            r.leave_in_parallel()
        perform_transaction(cb)

    run_parallel(f1, f2)

def test_abort_private_from_protected():
    p = nalloc(HDR + WORD)
    lib.setlong(p, 0, 897987)
    lib.stm_commit_transaction()
    lib.stm_begin_inevitable_transaction()
    #
    def cb(c):
        assert classify(p) == "protected"
        assert lib.getlong(p, 0) == 897987
        if c == 0:
            lib.setlong(p, 0, -38383)
            assert lib.getlong(p, 0) == -38383
            assert classify(p) == "private_from_protected"
            abort_and_retry()
    perform_transaction(cb)

def test_abort_stealing_while_modifying():
    test_stealing_while_modifying(aborting=True)

def test_stub_for_refs_from_stolen(old=False):
    p = palloc_refs(1)
    qlist = []
    qid = []
    pid = []
    def f1(r):
        q1 = nalloc(HDR + WORD)
        if old:
            lib.stm_push_root(q1)
            minor_collect()
            q1 = lib.stm_pop_root()
        assert (p.h_tid & GCFLAG_PUBLIC_TO_PRIVATE) == 0
        p1 = lib.stm_write_barrier(p)   # private copy
        assert p1 != p
        assert classify(p) == "public"
        assert classify(p1) == "private"
        assert p.h_tid & GCFLAG_PUBLIC_TO_PRIVATE
        qlist.append(q1)
        lib.setlong(q1, 0, -29187)
        lib.setptr(p1, 0, q1)
        lib.stm_commit_transaction()
        lib.stm_begin_inevitable_transaction()
        assert classify(p) == "public"
        assert classify(p1) == "protected"
        assert classify(follow_revision(p)) == "stub"
        assert p1.h_revision & 1
        pid.append(lib.stm_id(p1))
        assert classify(q1) == "protected"
        qid.append(lib.stm_id(q1))
        if old:
                assert q1.h_tid & GCFLAG_OLD
                assert not (q1.h_tid & GCFLAG_HAS_ID)
        else:
                assert not (q1.h_tid & GCFLAG_OLD)
                assert q1.h_tid & GCFLAG_HAS_ID
        
        r.set(2)
        r.wait(3)     # wait until the other thread really started
    def f2(r):
        r.wait(2)
        r.set(3)
        p2 = lib.stm_read_barrier(p)    # steals
        assert classify(p2) == "public"
        q2 = lib.getptr(p2, 0)
        assert lib.stm_id(p2) == pid[0]
        assert lib.stm_id(q2) == qid[0]
        assert q2 != ffi.NULL
        assert q2 != qlist[0]
        assert classify(q2) == "stub"
        assert q2.h_revision % 4 == 2
        q3 = lib.stm_read_barrier(q2)
        assert lib.stm_id(q3) == qid[0]
        assert q3 != q2
        if old:
            assert q3 == qlist[0]
        assert classify(q3) == "public"   # has been stolen
        assert lib.getlong(q3, 0) == -29187
    run_parallel(f1, f2)

def test_stub_for_refs_from_stolen_old():
    test_stub_for_refs_from_stolen(old=True)


def id_with_stealing(a, b, c):
    p = palloc_refs(2)
    qlist = []
    rlist = []
    plist = []
    qid = []
    pid = []
    rid = []
    def f1(r):
        r1 = nalloc(HDR)
        q1 = nalloc(HDR)
        p1 = lib.stm_write_barrier(p)   # private copy
        plist.append(p1)
        qlist.append(q1)
        rlist.append(r1)
        if c and a:
            # id on young priv
            qid.append(lib.stm_id(q1))
            assert q1.h_tid & GCFLAG_HAS_ID
        if c and b:
            # id on pub_to_priv
            assert follow_original(p1) == p
            pid.append(lib.stm_id(p1))
            assert not (p1.h_tid & GCFLAG_HAS_ID)
        lib.setptr(p1, 0, q1)
        lib.setptr(p1, 1, r1)
        lib.stm_commit_transaction()
        lib.stm_begin_inevitable_transaction()
        # p old public -> stub -> p1 young prot
        # q1 young prot, r1 young prot
        if c and not a:
            # id on young prot
            qid.append(lib.stm_id(q1))
            assert q1.h_tid & GCFLAG_HAS_ID
        if c and not b:
            # id on young prot
            assert follow_original(p1) == p
            pid.append(lib.stm_id(p1))
            assert not (p1.h_tid & GCFLAG_HAS_ID)
            
        r1w = lib.stm_write_barrier(r1)
        lib.setptr(p1, 1, r1w)
        assert classify(r1w) == "private_from_protected"
        assert not (r1w.h_tid & GCFLAG_OLD)
        if c:
                rid.append(lib.stm_id(r1w))
        # assert not (r1w.h_tid & GCFLAG_HAS_ID) # use backup <- XXX
        # assert follow_original(r1w) == follow_revision(r1w)
        
        r.set(2)
        r.wait(3)     # wait until the other thread really started
    def f2(r):
        r.wait(2)
        r.set(3)

        assert classify(plist[-1]) == "protected"
        p2 = lib.stm_read_barrier(plist[-1])    # steals
        if not c:
            pid.append(lib.stm_id(p2))
        assert pid[-1] == lib.stm_id(p2)

        r2 = lib.getptr(p2, 1)
        r3 = lib.stm_read_barrier(r2)
        if not c:
            rid.append(lib.stm_id(r3))
        assert lib.stm_id(r3) == rid[-1]
        
        q2 = lib.getptr(p2, 0)
        q3 = lib.stm_read_barrier(q2)
        if not c:
            qid.append(lib.stm_id(q3))

        assert lib.stm_id(q3) == qid[-1]
        assert lib.stm_id(q2) == qid[-1]

    run_parallel(f1, f2)

def test_id_with_stealing():
    id_with_stealing(1, 1, 0)
def test_id_with_stealing1():
    id_with_stealing(1, 0, 0)
def test_id_with_stealing2():
    id_with_stealing(0, 1, 0)
def test_id_with_stealing3():
    id_with_stealing(0, 0, 0)
def test_id_with_stealing4():
    id_with_stealing(1, 1, 1)
def test_id_with_stealing5():
    id_with_stealing(1, 0, 1)
def test_id_with_stealing6():
    id_with_stealing(0, 1, 1)
def test_id_with_stealing7():
    id_with_stealing(0, 0, 1)



def test_prehash_simple():
    p = palloc(HDR, 99)
    assert lib.stm_hash(p) == 99
    assert lib.stm_id(p) != lib.stm_hash(p)
    pr = lib.stm_read_barrier(p)
    assert lib.stm_hash(pr) == 99
    assert lib.stm_id(p) == lib.stm_id(pr)
    pw = lib.stm_write_barrier(p)
    assert lib.stm_hash(pw) == 99
    assert lib.stm_id(p) == lib.stm_id(pw)
    lib.stm_push_root(pw)
    minor_collect()
    pw = lib.stm_pop_root()
    assert lib.stm_hash(pw) == 99


def test_prehash_with_stealing():
    p = palloc(HDR, 99)
    def f1(r):
        lib.stm_write_barrier(p)   # private copy
        lib.stm_commit_transaction()
        lib.stm_begin_inevitable_transaction()
        # p old public -> stub -> p1 young prot
        r.set(2)
        r.wait(3)     # wait until the other thread really started
    def f2(r):
        r.wait(2)
        r.set(3)

        p2 = lib.stm_read_barrier(p)    # steals
        assert lib.stm_hash(p) == 99
        assert lib.stm_hash(p2) == 99
        assert lib.stm_id(p) == lib.stm_id(p2)
        assert lib.stm_hash(p) != lib.stm_id(p)
    run_parallel(f1, f2)

def test_enter_callback_call():
    lib.stm_commit_transaction()
    x = lib.stm_enter_callback_call()
    assert x == 0
    lib.stm_leave_callback_call(x)
    lib.stm_begin_inevitable_transaction()
    #
    lib.stm_finalize()
    x = lib.stm_enter_callback_call()
    assert x == 1
    lib.stm_leave_callback_call(x)
    lib.stm_initialize_tests(0)
