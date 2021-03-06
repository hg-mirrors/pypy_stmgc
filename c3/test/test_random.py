import py, random, thread, threading, time, collections
from support import *
import model

# a default seed that changes every day, but that can be easily recovered
DEFAULT_SEED = int(time.strftime("%y%m%d", time.gmtime()))


def setup_function(_):
    lib.stm_clear_between_tests()

class TransactionBreak(Exception):
    pass

class MissingAbort(Exception):
    pass

# pairs: "obj" is an StmObject from model.py, and "ptr" is a C cffi pointer
Pair = collections.namedtuple("Pair", "obj ptr")
def pair(obj, ptr):
    assert (obj is None) == (ptr == ffi.NULL)
    return Pair(obj, ptr)
emptypair = pair(None, ffi.cast("gcptr", 0))


class RandomSingleThreadTester(object):

    def __init__(self, seed=0, sync=None, sync_wait=None):
        sys.stdout.flush()
        self.roots = []
        self.rnd = random.Random(seed)
        self.seed = seed
        if sync is None:
            sync = Sync(shared=False)
        self.sync = sync
        self.sync_wait = sync_wait
        self.counter = 0
        self.current_rev = None
        self.expecting_gap_in_commit_time = 2
        self.dump('run')
        #
        if sync.prebuilt_object is None:
            lib.stm_initialize_tests(0)
            self.startrev()
            sync.prebuilt_object = self.alloc(palloc_refs)
            self.commit()
            lib.stm_finalize()
        self.prebuilt_object = sync.prebuilt_object
        self.roots.append(self.prebuilt_object)

    def dump(self, text):
        text = '%d.%d$ %s\n' % (self.seed, self.counter, text)
        sys.stderr.write(text)
        self.counter += 1
        #if text.startswith('261035.184$'):
        #    import pdb; pdb.set_trace()

    def check_not_free(self, ptr):
        assert ptr != ffi.NULL
        if ptr == self.prebuilt_object.ptr:
            check_prebuilt(ptr)
        else:
            check_not_free(ptr)

    def alloc(self, alloc_refs=nalloc_refs):
        self.push_roots()
        pid = lib.pseudoprebuilt(HDR, 42 + HDR)
        nptr = alloc_refs(3)
        self.pop_roots()
        assert rawgetptr(nptr, 0) == ffi.NULL
        assert rawgetptr(nptr, 1) == ffi.NULL
        rawsetptr(nptr, 2, pid)
        assert self.current_rev is not None
        obj = model.StmObject(self.current_rev, 2)
        obj.identity = pid
        self.sync.id2stmobj[pid] = obj
        return pair(obj, nptr)

    def check(self, p):
        assert isinstance(p, Pair)
        if p != emptypair and not is_stub(p.ptr):
            self.check_not_free(p.ptr)
            pid = lib.rawgetptr(p.ptr, 2)
            assert pid == p.obj.identity

    def expected_abort(self, manual=False):
        if manual:
            lib.stm_set_max_aborts(0)
        else:
            lib.stm_set_max_aborts(1)
        self.expected_conflict = True
        self.aborted_rev = self.current_rev
        self.current_rev = None

    def set_into_root(self, p, index):
        r = self._r
        if r != emptypair:
            self.check(r)
            self.check(p)
            try:
                self.current_rev.write(r.obj, index, p.obj)
                if not self.is_private(r.ptr):
                    self.current_rev.check_not_outdated(r.obj)
            except (model.Deleted, model.Conflict):
                # abort! try to reproduce with C code
                self.expected_abort()
                lib.setptr(r.ptr, index, p.ptr)   # should abort
                raise MissingAbort

            lib.setptr(r.ptr, index, p.ptr)   # must not abort
            self.possibly_update_time()
            self.dump('set_into_root(%r, %r, %r)' % (r.obj, index, p.obj))

    def get_ref(self, r, index):
        self.check(r)
        if r == emptypair:
            return emptypair
        try:
            pobj = self.current_rev.read(r.obj, index)
            if not self.is_private(r.ptr):
                self.current_rev.check_not_outdated(r.obj)
        except (model.Deleted, model.Conflict):
            # abort! try to reproduce with C code
            self.expected_abort()
            lib.getptr(r.ptr, index)             # should abort
            raise MissingAbort

        pptr = lib.getptr(r.ptr, index)
        self.possibly_update_time()
        p = pair(pobj, pptr)
        self.check(p)
        return p

    def validate(self, p):
        self.get_ref(p, 0)
        self.get_ref(p, 1)

    def read_barrier(self, p):
        if p != emptypair:
            self.check(p)
            try:
                self.current_rev.read_barrier(p.obj)
                if not self.is_private(p.ptr):
                    self.current_rev.check_not_outdated(p.obj)
            except (model.Deleted, model.Conflict):
                # abort! try to reproduce with C code
                self.expected_abort()
                lib.stm_read_barrier(p.ptr)          # should abort
                raise MissingAbort

            nptr = lib.stm_read_barrier(p.ptr)
            self.possibly_update_time()
            p = pair(p.obj, nptr)
            self.check(p)
        return p

    def write_barrier(self, p):
        if p != emptypair:
            self.check(p)
            try:
                self.current_rev.write_barrier(p.obj)
                if not self.is_private(p.ptr):
                    self.current_rev.check_not_outdated(p.obj)
            except (model.Deleted, model.Conflict):
                # abort! try to reproduce with C code
                self.expected_abort()
                lib.stm_write_barrier(p.ptr)         # should abort
                raise MissingAbort

            nptr = lib.stm_write_barrier(p.ptr)
            self.possibly_update_time()
            p = pair(p.obj, nptr)
            self.check(p)
        return p

    def push_roots(self, extra=emptypair):
        for r in self.roots + [extra]:
            self.check(r)
            lib.stm_push_root(r.ptr)

    def pop_roots(self, extra=emptypair):
        new = self.roots + [extra]
        for i in range(len(new)-1, -1, -1):
            r = new[i]
            nptr = lib.stm_pop_root()
            p = pair(r.obj, nptr)
            self.check(p)
            new[i] = p
        newextra = new.pop()
        self.roots[:] = new
        return newextra

    def nonrecord_barrier(self, ptr):
        result = ffi.new("int *")
        ptr = lib._stm_nonrecord_barrier(ptr, result)
        return ptr, result[0]

    def is_private(self, ptr):
        return ptr.h_revision == lib.get_local_revision()

    def check_valid(self, lst):
        lst = list(lst)
        seen = set(lst)
        while lst:
            p = lst.pop()
            if p == emptypair:
                continue
            self.check(p)

            ptr, result = self.nonrecord_barrier(p.ptr)
            if ptr == ffi.NULL and result == 3:
                continue    # can't check anything: we'd need foreign access
            has_private_copy = p.obj in self.current_rev.content
            assert has_private_copy == (result >= 1)
            if has_private_copy:
                content = self.current_rev.content[p.obj]
            else:
                try:
                    content = self.current_rev._try_read(p.obj)
                    is_too_recent = False
                except model.Deleted:
                    is_too_recent = True
                if result < 0:
                    assert is_too_recent
                if is_too_recent:
                    continue     # can't really check more in this case

            self.check_not_free(ptr)
            assert lib.rawgetptr(ptr, 2) == p.obj.identity

            for i in range(2):
                qobj = content[i]
                qptr = lib.rawgetptr(ptr, i)
                q = pair(qobj, qptr)
                self.check(q)
                if q not in seen:
                    lst.append(q)
                    seen.add(q)

    def transaction_break(self):
        if self.interruptible_transaction:
            raise TransactionBreak
        # start an interruptible transaction
        self.push_roots()
        self.commit()
        self.roots_outside_perform = self.roots[:]
        self.interruptible_transaction = True
        self.expected_conflict = False
        self.dump("callback_interruptible_transaction starting")
        perform_transaction(self.callback_interruptible_transaction)
        assert not self.expected_conflict
        self.interruptible_transaction = False
        self.roots = self.roots_outside_perform[:]
        self.startrev()
        self.pop_roots()
        del self.roots_outside_perform

    def callback_interruptible_transaction(self, retry_counter):
        self.dump("callback_interruptible_transaction(%d)" % retry_counter)
        #
        if retry_counter == 0:
            assert not self.expected_conflict
        else:
            assert self.expected_conflict
            self.expected_conflict = False
        #
        self.roots = self.roots_outside_perform[:]
        self.startrev()
        # XXX stm_perform_transaction() adds one root for the unused arg
        arg = lib.stm_pop_root()
        assert arg == ffi.NULL
        self.pop_roots()
        self.push_roots()
        lib.stm_push_root(arg)
        self.check_valid(self.roots)
        #
        try:
            self.run_me(self.sync_wait)
        except TransactionBreak:
            restart = (self.rnd.randrange(0, 3) != 1)
        else:
            restart = False
        #
        try:
            self.commit()
        except model.Conflict, e:
            self.dump("interruptible_transaction expecting %s" % (e,))
            if isinstance(e, model.ReadWriteConflict):
                self.expecting_gap_in_commit_time += 2
            self.expected_abort()
            return 0
        #
        self.expecting_gap_in_commit_time = 2
        if restart:
            self.dump("interruptible_transaction break")
            return 1
        self.dump("callback_interruptible_transaction finished")
        return 0

    def startrev(self):
        assert self.current_rev is None
        self.current_rev = model.Revision(self.sync.gs)
        t = lib.get_start_time()
        self.current_rev.start_time = t
        if self.current_rev.previous is not None:
            t_prev = t - self.expecting_gap_in_commit_time
            if hasattr(self.current_rev.previous, 'commit_time'):
                assert self.current_rev.previous.commit_time == t_prev
            else:
                self.current_rev.previous.commit_time = t_prev

    def possibly_update_time(self):
        t = lib.get_start_time()
        t_prev = t - self.expecting_gap_in_commit_time
        assert self.current_rev.previous.commit_time == t_prev
        self.current_rev.start_time = t

    def commit(self):
        assert self.current_rev.start_time == lib.get_start_time()
        self.current_rev.commit_transaction()
        self.current_rev = None

    def run_me(self, do_wait):
        p = emptypair
        while self.steps_remaining > 0:
            self.steps_remaining -= 1
            p = self.run_one_step(p)
            assert isinstance(p, Pair)
            #
            if do_wait:
                self.push_roots(extra=p)
                do_wait()
                p = self.pop_roots(extra=p)

    def run_single_thread(self):
        lib.stm_initialize_tests(0)
        self.interruptible_transaction = False
        self.startrev()
        #
        self.steps_remaining = 10000
        #
        self.run_me(do_wait=False)
        #
        self.commit()
        lib.stm_finalize()

    def run_one_step(self, p):
        num = self.rnd.randrange(0, len(self.roots))
        self._r = self.roots[num]
        assert isinstance(self._r, Pair)
        k = self.rnd.randrange(0, 13)
        self.dump('%4s%4s' % (k, num))
        #
        if k == 0:     # remove a root
            if num > 0:
                del self.roots[num]
        elif k == 1:   # set 'p' to point to a root
            if self._r:
                p = self._r
        elif k == 2:   # add 'p' to the roots
            self.check(p)
            self.roots.append(p)
        elif k == 3:   # allocate a fresh 'p'
            p = self.alloc()
        elif k == 4:   # set 'p' as refs[0] in one of the roots
            self.set_into_root(p, 0)
        elif k == 5:   # set 'p' as refs[1] in one of the roots
            self.set_into_root(p, 1)
        elif k == 6:   # read and validate 'p'
            self.validate(p)
        elif k == 7:   # transaction break
            self.transaction_break()
            p = emptypair
        elif k == 8:   # only do an stm_write_barrier
            p = self.write_barrier(p)
        elif k == 9:   # set 'p' to 'p.refs[0]'
            p = self.get_ref(p, 0)
        elif k == 10:  # set 'p' to 'p.refs[1]'
            p = self.get_ref(p, 1)
        elif k == 11:  # rare events
            k1 = self.rnd.randrange(0, 100)
            if k1 == 7:
                self.dump('major collect')
                self.push_roots()
                major_collect()
                self.pop_roots()
                p = emptypair
            if k1 == 82 and self.interruptible_transaction:
                self.dump('~~~~~~~~~~~~~~~~~~~~ ABORT ~~~~~~~~~~~~~~~~~~~~')
                self.expected_abort(manual=True)
                abort_and_retry()
        elif k == 12:   # only do an stm_read_barrier
            p = self.read_barrier(p)
        self.dump('-')
        self.check_valid(self.roots + [p])
        return p


def test_single_thread(seed=DEFAULT_SEED):
    tester = RandomSingleThreadTester(seed=seed)
    tester.run_single_thread()

def test_more_single_thread():
    py.test.skip("more random tests")
    for i in range(100):
        yield test_single_thread, i + 3900


class Sync(object):
    def __init__(self, shared):
        self.shared = shared
        self.toggle = False
        self.cond = threading.Condition()
        self.finished = False
        self.prebuilt_object = None
        self.id2stmobj = {}
        self.gs = model.GlobalState()

    def wait(self, threadid):
        lib.stm_stop_sharedlock()
        while self.toggle != threadid and not self.finished:
            self.cond.wait()
        self.toggle = not threadid
        self.cond.notify()
        lib.stm_start_sharedlock()

    def wait1(self):
        self.wait(False)

    def wait2(self):
        self.wait(True)


def test_multi_thread(seed=DEFAULT_SEED):
    #py.test.skip("in-progress")
    sync = Sync(shared=True)
    tester1 = RandomSingleThreadTester(seed * 2 + 0, sync, sync.wait1)
    tester2 = RandomSingleThreadTester(seed * 2 + 1, sync, sync.wait2)
    #
    done1 = thread.allocate_lock(); done1.acquire()
    done2 = thread.allocate_lock(); done2.acquire()
    fine = []
    #
    def subt(tester, done):
        try:
            tester.dump("thread starting")
            sync.cond.acquire()
            try:
                tester.run_single_thread()
            except Exception:
                if not sys.stdout.isatty():
                    raise
                import pdb; pdb.post_mortem(sys.exc_info()[2])
                raise
            fine.append(True)
        finally:
            e = sys.exc_info()[1]
            if e is None:
                with_exc = ''
            else:
                with_exc = 'with %s: %s' % (e.__class__.__name__, e)
            tester.dump("thread stopping %s" % with_exc)
            sync.finished = True
            sync.cond.notify()
            sync.cond.release()
            done.release()

    thread.start_new_thread(subt, (tester1, done1))
    thread.start_new_thread(subt, (tester2, done2))
    done1.acquire()
    done2.acquire()
    assert fine == [True, True]


def test_more_multi_thread():
    py.test.skip("more random tests")
    for i in range(2, 1000):
        yield test_multi_thread, i
