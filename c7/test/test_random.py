from support import *
import sys, random
import py
from cStringIO import StringIO




class Exec(object):
    def __init__(self, test):
        self.content = {'self': test}
        self.thread_num = 0

    def do(self, cmd):
        color = "\033[%dm" % (31 + self.thread_num % 6)
        print >> sys.stderr, color + cmd + "\033[0m"
        exec cmd in globals(), self.content


def raising_call(conflict, func, *args):
    arguments = ", ".join(map(str, args))
    if conflict:
        return "py.test.raises(Conflict, %s, %s)" % (func, arguments)
    return "%s(%s)" % (func, arguments)


class WriteWriteConflictNotTestable(Exception):
    # How can I test a write-write conflict between
    # an inevitable and a normal transaction? The
    # inevitable transaction would have to wait,
    # but now for tests we simply abort. Of course
    # aborting the inevitable transaction is not possible..
    pass

def contention_management(our_trs, other_trs, wait=False, objs_in_conflict=None):
    """exact copy of logic in contention.c"""
    if our_trs.inevitable and wait:
        # we win but cannot wait in tests...
        raise WriteWriteConflictNotTestable

    if our_trs.inevitable:
        other_trs.set_must_abort(objs_in_conflict)
    elif other_trs.start_time < our_trs.start_time:
        pass
    elif not other_trs.inevitable:
        other_trs.set_must_abort(objs_in_conflict)

    if not other_trs.check_must_abort():
        our_trs.set_must_abort(objs_in_conflict)
    elif wait:
        assert not our_trs.inevitable
        # abort anyway:
        our_trs.set_must_abort(objs_in_conflict)


class TransactionState(object):
    """State of a transaction running in a thread,
    e.g. maintains read/write sets. The state will be
    discarded on abort or pushed to other threads"""

    def __init__(self, start_time):
        self.read_set = set()
        self.write_set = set()
        self.values = {}
        self._must_abort = False
        self.start_time = start_time
        self.objs_in_conflict = set()
        self.inevitable = False
        self.created_in_this_transaction = set()

    def get_old_modified(self):
        # returns only the ones that are modified and not from
        # this transaction
        return self.write_set.difference(self.created_in_this_transaction)

    def set_must_abort(self, objs_in_conflict=None):
        assert not self.inevitable
        if objs_in_conflict is not None:
            self.objs_in_conflict |= objs_in_conflict
        self._must_abort = True

    def check_must_abort(self):
        return self._must_abort

    def has_conflict_with(self, committed):
        return bool(self.read_set & committed.write_set)

    def update_from_committed(self, committed, only_new=False):
        """returns True if conflict"""
        if only_new:
            for w in committed.write_set:
                self.values[w] = committed.values[w]
        else:
            self.values.update(committed.values)

        if self.has_conflict_with(committed):
            contention_management(self, committed,
                                  objs_in_conflict=self.read_set & committed.write_set)
        return self.check_must_abort()

    def read_root(self, r):
        self.read_set.add(r)
        return self.values[r]

    def add_root(self, r, v, created_in_this_transaction):
        assert self.values.get(r, None) is None
        self.values[r] = v
        if created_in_this_transaction:
            self.created_in_this_transaction.add(r)

    def write_root(self, r, v):
        self.read_set.add(r)
        self.write_set.add(r)
        old = self.values.get(r, None)
        self.values[r] = v
        return old


class ThreadState(object):
    """Maintains state for one thread. Mostly manages things
    to be kept between transactions (e.g. saved roots) and
    handles discarding/reseting states on transaction abort"""

    def __init__(self, num, global_state):
        self.num = num
        self.saved_roots = []
        self.roots_on_stack = 0
        self.roots_on_transaction_start = 0
        self.transaction_state = None
        self.global_state = global_state

    def register_root(self, r):
        self.saved_roots.append(r)
        assert len(self.saved_roots) < SHADOWSTACK_LENGTH

    def forget_random_root(self):
        # # forget some non-pushed root for now
        # if self.roots_on_stack < len(self.saved_roots):
        #     idx = self.global_state.rnd.randrange(self.roots_on_stack, len(self.saved_roots))
        #     r = self.saved_roots[idx]
        #     del self.saved_roots[idx]
        #     return r

        if self.transaction_state.inevitable:
            # forget *all* roots
            self.roots_on_stack = 0
            self.roots_on_transaction_start = 0
            res = str(self.saved_roots)
            del self.saved_roots[:]
        else:
            # forget all non-pushed roots for now
            assert self.roots_on_stack == self.roots_on_transaction_start
            res = str(self.saved_roots[self.roots_on_stack:])
            del self.saved_roots[self.roots_on_stack:]
        return res

    def get_random_root(self):
        rnd = self.global_state.rnd
        if self.saved_roots:
            return rnd.choice([rnd.choice(self.global_state.prebuilt_roots),
                               rnd.choice(self.saved_roots)])
        return rnd.choice(self.global_state.prebuilt_roots)

    def push_roots(self, ex):
        assert self.roots_on_stack == self.roots_on_transaction_start
        for r in self.saved_roots[self.roots_on_transaction_start:]:
            ex.do('self.push_root(%s)' % r)
            self.roots_on_stack += 1

    def pop_roots(self, ex):
        for r in reversed(self.saved_roots[self.roots_on_transaction_start:]):
            ex.do('%s = self.pop_root()' % r)
            self.roots_on_stack -= 1
        assert self.roots_on_stack == self.roots_on_transaction_start

    def reload_roots(self, ex):
        assert self.roots_on_stack == self.roots_on_transaction_start
        to_reload = self.saved_roots[:self.roots_on_stack]
        if to_reload:
            ex.do("# reload roots on stack:")
            for r in reversed(to_reload):
                ex.do('%s = self.pop_root()' % r)
            for r in to_reload:
                ex.do('self.push_root(%s)' % r)

    def start_transaction(self):
        assert self.transaction_state is None
        start_time = self.global_state.inc_and_get_global_time()
        trs = TransactionState(start_time)
        trs.update_from_committed(
            self.global_state.committed_transaction_state)
        self.transaction_state = trs
        self.roots_on_transaction_start = self.roots_on_stack

    def commit_transaction(self):
        trs = self.transaction_state
        gtrs = self.global_state.committed_transaction_state
        self.global_state.check_for_write_read_conflicts(trs)
        conflicts = trs.check_must_abort()
        if not conflicts:
            # update global committed state w/o conflict
            assert not gtrs.update_from_committed(trs)
            self.global_state.push_state_to_other_threads(trs)
            self.transaction_state = None
        return conflicts

    def abort_transaction(self):
        assert self.transaction_state.check_must_abort()
        self.roots_on_stack = self.roots_on_transaction_start
        del self.saved_roots[self.roots_on_stack:]
        self.transaction_state = None


class GlobalState(object):
    """Maintains the global view (in a TransactionState) on
    objects and threads. It also handles checking for conflicts
    between threads and pushing state to other threads"""

    def __init__(self, ex, rnd):
        self.ex = ex
        self.rnd = rnd
        self.thread_states = []
        self.prebuilt_roots = []
        self.committed_transaction_state = TransactionState(0)
        self.global_time = 0
        self.root_numbering = 0
        self.ref_type_map = {}
        self.root_sizes = {}

    def get_new_root_name(self, is_ref_type, size):
        self.root_numbering += 1
        r = "lp_%s_%d" % ("ref" if is_ref_type else "char", self.root_numbering)
        self.ref_type_map[r] = is_ref_type
        self.root_sizes[r] = size
        return r

    def has_ref_type(self, r):
        return self.ref_type_map[r]

    def get_root_size(self, r):
        return self.root_sizes[r]

    def inc_and_get_global_time(self):
        self.global_time += 1
        return self.global_time

    def push_state_to_other_threads(self, trs):
        assert not trs.check_must_abort()
        for ts in self.thread_states:
            other_trs = ts.transaction_state
            if other_trs is None or other_trs is trs:
                continue
            other_trs.update_from_committed(trs, only_new=True)

        if trs.check_must_abort():
            self.ex.do('# conflict while pushing to other threads: %s' %
                       trs.objs_in_conflict)

    def check_if_can_become_inevitable(self, trs):
        assert not trs.check_must_abort()
        for ts in self.thread_states:
            other_trs = ts.transaction_state
            if (other_trs and trs is not other_trs
                and other_trs.inevitable):
                self.ex.do("# there is another inevitable transaction:")
                trs.set_must_abort()
                break

    def check_for_write_write_conflicts(self, trs):
        assert not trs.check_must_abort()
        for ts in self.thread_states:
            other_trs = ts.transaction_state
            if other_trs is None or other_trs is trs:
                continue

            confl_set = other_trs.write_set & trs.write_set
            if confl_set:
                contention_management(trs, other_trs, True,
                                      objs_in_conflict=confl_set)

        if trs.check_must_abort():
            self.ex.do('# write-write conflict: %s' %
                       trs.objs_in_conflict)

    def check_for_write_read_conflicts(self, trs):
        assert not trs.check_must_abort()
        for ts in self.thread_states:
            other_trs = ts.transaction_state
            if other_trs is None or other_trs is trs:
                continue

            confl_set = other_trs.read_set & trs.write_set
            if confl_set:
                contention_management(trs, other_trs,
                                      objs_in_conflict=confl_set)

        if trs.check_must_abort():
            self.ex.do('# write-read conflict: %s' %
                       trs.objs_in_conflict)


# ========== STM OPERATIONS ==========

class Operation(object):
    def do(self, ex, global_state, thread_state):
        raise NotImplemented

class OpStartTransaction(Operation):
    def do(self, ex, global_state, thread_state):
        thread_state.start_transaction()
        #
        ex.do('self.start_transaction()')
        thread_state.reload_roots(ex)
        #
        # assert that everything known is old:
        old_objs = thread_state.saved_roots
        for o in old_objs:
            ex.do("assert not is_in_nursery(%s)" % o)


class OpCommitTransaction(Operation):
    def do(self, ex, global_state, thread_state):
        #
        # push all new roots
        ex.do("# push new objs before commit:")
        thread_state.push_roots(ex)
        aborts = thread_state.commit_transaction()
        #
        if aborts:
            thread_state.abort_transaction()
        ex.do(raising_call(aborts, "self.commit_transaction"))

class OpAbortTransaction(Operation):
    def do(self, ex, global_state, thread_state):
        trs = thread_state.transaction_state
        if trs.inevitable:
            return
        trs.set_must_abort()
        thread_state.abort_transaction()
        ex.do('self.abort_transaction()')

class OpBecomeInevitable(Operation):
    def do(self, ex, global_state, thread_state):
        trs = thread_state.transaction_state
        global_state.check_if_can_become_inevitable(trs)

        thread_state.push_roots(ex)
        ex.do(raising_call(trs.check_must_abort(),
                           "stm_become_inevitable"))
        if trs.check_must_abort():
            thread_state.abort_transaction()
        else:
            trs.inevitable = True
            thread_state.pop_roots(ex)
            thread_state.reload_roots(ex)


class OpAllocate(Operation):
    def do(self, ex, global_state, thread_state):
        size = global_state.rnd.choice([
            "16",
            "SOME_MEDIUM_SIZE+16",
            #"SOME_LARGE_SIZE+16",
        ])
        r = global_state.get_new_root_name(False, size)
        thread_state.push_roots(ex)

        ex.do('%s = stm_allocate(%s)' % (r, size))
        thread_state.transaction_state.add_root(r, 0, True)

        thread_state.pop_roots(ex)
        thread_state.reload_roots(ex)
        thread_state.register_root(r)

class OpAllocateRef(Operation):
    def do(self, ex, global_state, thread_state):
        num = str(global_state.rnd.randrange(1, 100))
        r = global_state.get_new_root_name(True, num)
        thread_state.push_roots(ex)
        ex.do('%s = stm_allocate_refs(%s)' % (r, num))
        thread_state.transaction_state.add_root(r, "ffi.NULL", True)

        thread_state.pop_roots(ex)
        thread_state.reload_roots(ex)
        thread_state.register_root(r)

class OpMinorCollect(Operation):
    def do(self, ex, global_state, thread_state):
        thread_state.push_roots(ex)
        ex.do('stm_minor_collect()')
        thread_state.pop_roots(ex)
        thread_state.reload_roots(ex)


class OpForgetRoot(Operation):
    def do(self, ex, global_state, thread_state):
        r = thread_state.forget_random_root()
        if thread_state.transaction_state.inevitable:
            ex.do('# inevitable forget %s' % r)
        else:
            ex.do('# forget %s' % r)

class OpWrite(Operation):
    def do(self, ex, global_state, thread_state):
        r = thread_state.get_random_root()
        trs = thread_state.transaction_state
        is_ref = global_state.has_ref_type(r)
        #
        # check for possible write-write conflict:
        was_written = False
        try:
            # HACK to avoid calling write_root() just yet because we have to
            # undo it in case of the exception :(
            was_written = r in trs.write_set
            trs.write_set.add(r)
            global_state.check_for_write_write_conflicts(trs)
        except WriteWriteConflictNotTestable:
            if not was_written:
                trs.write_set.remove(r)
            ex.do("# writing to %s produces an untestable write-write" % r)
            ex.do("# conflict between an inevitable and a normal transaction :(")
            return
        #
        # decide on a value to write
        if is_ref:
            v = thread_state.get_random_root()
        else:
            v = ord(global_state.rnd.choice("abcdefghijklmnop"))
        assert trs.write_root(r, v) is not None
        #
        aborts = trs.check_must_abort()
        if aborts:
            thread_state.abort_transaction()
        offset = global_state.get_root_size(r) + " - 1"
        if is_ref:
            ex.do(raising_call(aborts, "stm_set_ref", r, offset, v))
            if not aborts:
                ex.do(raising_call(False, "stm_set_ref", r, "0", v))
        else:
            ex.do(raising_call(aborts, "stm_set_char", r, repr(chr(v)), offset))
            if not aborts:
                ex.do(raising_call(False, "stm_set_char", r, repr(chr(v)), "HDR"))

class OpRead(Operation):
    def do(self, ex, global_state, thread_state):
        r = thread_state.get_random_root()
        trs = thread_state.transaction_state
        v = trs.read_root(r)
        #
        offset = global_state.get_root_size(r) + " - 1"
        if global_state.has_ref_type(r):
            if v in thread_state.saved_roots or v in global_state.prebuilt_roots:
                # v = root known to this transaction; or prebuilt
                ex.do("assert stm_get_ref(%s, %s) == %s" % (r, offset, v))
                ex.do("assert stm_get_ref(%s, 0) == %s" % (r, v))
            elif v != "ffi.NULL":
                # if v came from this transaction: re-add it to saved_roots because
                #     it survived by being referenced by another saved root
                # if v is from a different transaction:
                #     we fish its value from somewhere and add it to our known roots
                global_trs = global_state.committed_transaction_state
                if v not in trs.values:
                    # not from this transaction AND not known at the start of this
                    # transaction
                    trs.add_root(v, global_trs.values[v], False)
                    ex.do("# get %r from other thread" % v)
                elif v not in global_trs.values:
                    # created and forgotten earlier in this thread
                    ex.do("# revive %r in this thread" % v)
                else:
                    # created in an earlier transaction, now also known here
                    ex.do("# register %r in this thread" % v)
                #
                ex.do("%s = stm_get_ref(%s, %s)" % (v, r, offset))
                ex.do("%s = stm_get_ref(%s, 0)" % (v, r))
                thread_state.register_root(v)
            else:
                # v is NULL; we still need to read it (as it should be in the read-set):
                ex.do("assert stm_get_ref(%s, %s) == %s" % (r,offset,v))
                ex.do("assert stm_get_ref(%s, 0) == %s" % (r,v))
        else:
            ex.do("assert stm_get_char(%s, %s) == %s" % (r, offset, repr(chr(v))))
            ex.do("assert stm_get_char(%s, HDR) == %s" % (r, repr(chr(v))))

class OpAssertSize(Operation):
    def do(self, ex, global_state, thread_state):
        r = thread_state.get_random_root()
        size = global_state.get_root_size(r)
        if global_state.has_ref_type(r):
            ex.do("assert stm_get_obj_size(%s) == %s" % (r, size + " * WORD + HDR"))
        else:
            ex.do("assert stm_get_obj_size(%s) == %s" % (r, size))

class OpAssertModified(Operation):
    def do(self, ex, global_state, thread_state):
        trs = thread_state.transaction_state
        modified = trs.get_old_modified()
        ex.do("# modified = %s" % modified)
        ex.do("modified = modified_objects()")
        if not modified:
            ex.do("assert modified == []")
        else:
            saved = [m for m in modified
                     if m in thread_state.saved_roots or m in global_state.prebuilt_roots]
            ex.do("assert {%s}.issubset(set(modified))" % (
                ", ".join(saved)
            ))


class OpSwitchThread(Operation):
    def do(self, ex, global_state, thread_state, new_thread_state=None):
        if new_thread_state is None:
            new_thread_state = global_state.rnd.choice(global_state.thread_states)

        if new_thread_state != thread_state:
            if thread_state.transaction_state:
                thread_state.push_roots(ex)
            ex.do('#')
            #
            trs = new_thread_state.transaction_state
            conflicts = trs is not None and trs.check_must_abort()
            ex.thread_num = new_thread_state.num
            #
            ex.do(raising_call(conflicts,
                               "self.switch", new_thread_state.num))
            if conflicts:
                new_thread_state.abort_transaction()
            else:
                new_thread_state.pop_roots(ex)
                new_thread_state.reload_roots(ex)

        return new_thread_state



# ========== TEST GENERATION ==========

class TestRandom(BaseTest):

    def test_fixed_16_bytes_objects(self, seed=1010):
        rnd = random.Random(seed)

        N_OBJECTS = 3
        N_THREADS = 2
        ex = Exec(self)
        ex.do("################################################################\n"*10)
        ex.do('# initialization')

        global_state = GlobalState(ex, rnd)
        for i in range(N_THREADS):
            global_state.thread_states.append(
                ThreadState(i, global_state))
        curr_thread = global_state.thread_states[0]

        for i in range(N_OBJECTS):
            r = global_state.get_new_root_name(False, "384")
            ex.do('%s = stm_allocate_old(384)' % r)
            global_state.committed_transaction_state.add_root(r, 0, False)
            global_state.prebuilt_roots.append(r)

            r = global_state.get_new_root_name(True, "50")
            ex.do('%s = stm_allocate_old_refs(50)' % r)
            global_state.committed_transaction_state.add_root(r, "ffi.NULL", False)
            global_state.prebuilt_roots.append(r)
        global_state.committed_transaction_state.write_set = set()
        global_state.committed_transaction_state.read_set = set()

        # random steps:
        possible_actions = [
            OpAllocate,
            OpAllocateRef, OpAllocateRef,
            OpWrite, OpWrite, OpWrite,
            OpRead, OpRead, OpRead, OpRead, OpRead, OpRead, OpRead, OpRead,
            OpCommitTransaction,
            OpAbortTransaction,
            OpForgetRoot,
            OpBecomeInevitable,
            OpAssertSize,
            #OpAssertModified,
            OpMinorCollect,
        ]
        for _ in range(200):
            # make sure we are in a transaction:
            curr_thread = OpSwitchThread().do(ex, global_state, curr_thread)

            if curr_thread.transaction_state is None:
                OpStartTransaction().do(ex, global_state, curr_thread)

            # do something random
            action = rnd.choice(possible_actions)
            action().do(ex, global_state, curr_thread)

        # to make sure we don't have aborts in the test's teardown method,
        # we will simply stop all running transactions
        for ts in global_state.thread_states:
            if ts.transaction_state is not None:
                if curr_thread != ts:
                    ex.do('#')
                    curr_thread = OpSwitchThread().do(ex, global_state, curr_thread,
                                                      new_thread_state=ts)

                # could have aborted in the switch() above:
                if curr_thread.transaction_state:
                    OpCommitTransaction().do(ex, global_state, curr_thread)



    def _make_fun(seed):
        def test_fun(self):
            self.test_fixed_16_bytes_objects(seed)
        test_fun.__name__ = 'test_random_%d' % seed
        return test_fun

    for _seed in range(5000, 5100):
        _fn = _make_fun(_seed)
        locals()[_fn.__name__] = _fn
