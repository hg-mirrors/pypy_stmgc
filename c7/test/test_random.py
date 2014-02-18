from support import *
import sys, random
import py
from cStringIO import StringIO


class Exec(object):
    def __init__(self, test):
        self.content = {'self': test}

    def do(self, cmd):
        print >> sys.stderr, cmd
        exec cmd in globals(), self.content

_root_numbering = 0
def get_new_root_name():
    global _root_numbering
    _root_numbering += 1
    return "lp%d" % _root_numbering

_global_time = 0
def contention_management(our_trs, other_trs, wait=False):
    if other_trs.start_time < our_trs.start_time:
        pass
    else:
        other_trs.must_abort = True
        
    if not other_trs.must_abort:
        our_trs.must_abort = True
    elif wait:
        # abort anyway:
        our_trs.must_abort = True
        

class TransactionState(object):
    """maintains read/write sets"""
    def __init__(self, start_time):
        self.read_set = set()
        self.write_set = set()
        self.values = {}
        self.must_abort = False
        self.start_time = start_time

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
            contention_management(self, committed)
        return self.must_abort

    def read_root(self, r):
        self.read_set.add(r)
        return self.values[r]
    
    def write_root(self, r, v):
        self.read_set.add(r)
        self.write_set.add(r)
        old = self.values.get(r, None)
        self.values[r] = v
        return old
        

class ThreadState(object):
    """maintains state for one thread """
    
    def __init__(self, num, global_state):
        self.num = num
        self.saved_roots = []
        self.roots_on_stack = 0
        self.roots_on_transaction_start = 0
        self.transaction_state = None
        self.global_state = global_state

    def register_root(self, r):
        self.saved_roots.append(r)

    def forget_random_root(self):
        # # forget some non-pushed root for now
        # if self.roots_on_stack < len(self.saved_roots):
        #     idx = self.global_state.rnd.randrange(self.roots_on_stack, len(self.saved_roots))
        #     r = self.saved_roots[idx]
        #     del self.saved_roots[idx]
        #     return r
        
        # forget all non-pushed roots for now
        res = str(self.saved_roots[self.roots_on_stack:])
        del self.saved_roots[self.roots_on_stack:]
        return res

    def get_random_root(self):
        rnd = self.global_state.rnd
        if self.saved_roots:
            return rnd.choice([rnd.choice(self.global_state.shared_roots),
                               rnd.choice(self.saved_roots)])
        return rnd.choice(self.global_state.shared_roots)

    def push_roots(self, ex):
        for r in self.saved_roots[self.roots_on_transaction_start:]:
            ex.do('self.push_root(%s)' % r)
            self.roots_on_stack += 1
    
    def pop_roots(self, ex):
        for r in reversed(self.saved_roots[self.roots_on_transaction_start:]):
            ex.do('%s = self.pop_root()' % r)
            self.roots_on_stack -= 1

    def update_roots(self, ex):
        assert self.roots_on_stack == self.roots_on_transaction_start
        for r in self.saved_roots[::-1]:
            ex.do('%s = self.pop_root()' % r)
            self.roots_on_stack -= 1
        assert self.roots_on_stack == 0
        for r in self.saved_roots:
            ex.do('self.push_root(%s)' % r)
            self.roots_on_stack += 1

    def start_transaction(self):
        assert self.transaction_state is None
        global _global_time
        _global_time += 1
        start_time = _global_time
        trs = TransactionState(start_time)
        trs.update_from_committed(
            self.global_state.committed_transaction_state)
        self.transaction_state = trs
        self.roots_on_transaction_start = self.roots_on_stack
        
    def commit_transaction(self):
        trs = self.transaction_state
        gtrs = self.global_state.committed_transaction_state
        self.global_state.check_for_write_read_conflicts(trs)
        conflicts = trs.must_abort
        if not conflicts:
            # update global committed state w/o conflict
            assert not gtrs.update_from_committed(trs)
            self.global_state.push_state_to_other_threads(trs)
            self.transaction_state = None
        return conflicts

    def abort_transaction(self):
        assert self.transaction_state.must_abort
        self.roots_on_stack = self.roots_on_transaction_start
        del self.saved_roots[self.roots_on_stack:]
        self.transaction_state = None

        
class GlobalState(object):
    def __init__(self, rnd):
        self.rnd = rnd
        self.thread_states = []
        self.shared_roots = []
        self.committed_transaction_state = TransactionState(0)

    def push_state_to_other_threads(self, tr_state):
        for ts in self.thread_states:
            other_trs = ts.transaction_state
            if other_trs is None or other_trs is tr_state:
                continue
            other_trs.update_from_committed(tr_state, only_new=True)

    def check_for_write_write_conflicts(self, tr_state):
        for ts in self.thread_states:
            other_trs = ts.transaction_state
            if other_trs is None or other_trs is tr_state:
                continue
            
            if other_trs.write_set & tr_state.write_set:
                contention_management(tr_state, other_trs, True)

    def check_for_write_read_conflicts(self, tr_state):
        for ts in self.thread_states:
            other_trs = ts.transaction_state
            if other_trs is None or other_trs is tr_state:
                continue
            
            if other_trs.read_set & tr_state.write_set:
                contention_management(tr_state, other_trs)


# ========== STM OPERATIONS ==========

class Operation(object):
    def do(self, ex, global_state, thread_state):
        raise NotImplemented

class OpStartTransaction(Operation):
    def do(self, ex, global_state, thread_state):
        thread_state.start_transaction()
        #
        ex.do('self.start_transaction()')
        thread_state.update_roots(ex)


class OpCommitTransaction(Operation):
    def do(self, ex, global_state, thread_state):
        #
        # push all new roots
        thread_state.push_roots(ex)
        aborts = thread_state.commit_transaction()
        #
        if aborts:
            thread_state.abort_transaction()
            ex.do('py.test.raises(Conflict, self.commit_transaction)')
        else:
            ex.do('self.commit_transaction()')
            
class OpAllocate(Operation):
    def do(self, ex, global_state, thread_state):
        r = get_new_root_name()
        thread_state.push_roots(ex)
        ex.do('%s = stm_allocate(16)' % r)
        assert thread_state.transaction_state.write_root(r, 0) is None
        
        thread_state.pop_roots(ex)
        thread_state.register_root(r)

class OpForgetRoot(Operation):
    def do(self, ex, global_state, thread_state):
        r = thread_state.forget_random_root()
        ex.do('# forget %s' % r)

class OpSetChar(Operation):
    def do(self, ex, global_state, thread_state):
        r = thread_state.get_random_root()
        v = ord(global_state.rnd.choice("abcdefghijklmnop"))
        trs = thread_state.transaction_state
        trs.write_root(r, v)

        global_state.check_for_write_write_conflicts(trs)
        if trs.must_abort:
            thread_state.abort_transaction()
            ex.do("py.test.raises(Conflict, stm_set_char, %s, %s)" % (r, repr(chr(v))))
        else:
            ex.do("stm_set_char(%s, %s)" % (r, repr(chr(v))))

class OpGetChar(Operation):
    def do(self, ex, global_state, thread_state):
        r = thread_state.get_random_root()
        trs = thread_state.transaction_state
        v = trs.read_root(r)
        #
        ex.do("assert stm_get_char(%s) == %s" % (r, repr(chr(v))))

class OpSwitchThread(Operation):
    def do(self, ex, global_state, thread_state):
        trs = thread_state.transaction_state
        conflicts = trs is not None and trs.must_abort
        #
        if conflicts:
            thread_state.abort_transaction()
            ex.do('py.test.raises(Conflict, self.switch, %s)' % thread_state.num)
        else:
            ex.do('self.switch(%s)' % thread_state.num)
    

# ========== TEST GENERATION ==========
        
class TestRandom(BaseTest):

    def test_fixed_16_bytes_objects(self, seed=1010):
        rnd = random.Random(seed)

        N_OBJECTS = 5
        N_THREADS = 2
        ex = Exec(self)
        ex.do('# initialization')

        global_state = GlobalState(rnd)
        for i in range(N_THREADS):
            global_state.thread_states.append(
                ThreadState(i, global_state))
        curr_thread = global_state.thread_states[0]

        for i in range(N_OBJECTS):
            r = get_new_root_name()
            ex.do('%s = stm_allocate_old(16)' % r)
            global_state.committed_transaction_state.write_root(r, 0)
            global_state.shared_roots.append(r)
        global_state.committed_transaction_state.write_set = set()
        global_state.committed_transaction_state.read_set = set()

        # random steps:
        remaining_steps = 200
        while remaining_steps > 0:
            remaining_steps -= 1

            n_thread = rnd.randrange(0, N_THREADS)
            if n_thread != curr_thread.num:
                ex.do('#')
                curr_thread = global_state.thread_states[n_thread]
                OpSwitchThread().do(ex, global_state, curr_thread)
            if curr_thread.transaction_state is None:
                OpStartTransaction().do(ex, global_state, curr_thread)

            action = rnd.choice([
                OpAllocate,
                OpSetChar,
                OpSetChar,
                OpGetChar,
                OpGetChar,
                OpCommitTransaction,
                OpForgetRoot,
            ])
            action().do(ex, global_state, curr_thread)
            


    def _make_fun(seed):
        def test_fun(self):
            self.test_fixed_16_bytes_objects(seed)
        test_fun.__name__ = 'test_fixed_16_bytes_objects_%d' % seed
        return test_fun

    for _seed in range(5000, 5100):
        _fn = _make_fun(_seed)
        locals()[_fn.__name__] = _fn
