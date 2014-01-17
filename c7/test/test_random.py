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


class TestRandom(BaseTest):

    def test_fixed_16_bytes_objects(self, seed=1010):
        rnd = random.Random(seed)

        N_OBJECTS = 5
        N_THREADS = 2
        ex = Exec(self)
        ex.do('# initialization')
        ex.do('stm_start_transaction()')
        head_state = []
        for i in range(N_OBJECTS):
            ex.do('lp%d = stm_allocate(16)' % i)
            ex.do('stm_set_char(lp%d, %r)' % (i, chr(i)))
            head_state.append(chr(i))
            ex.do('stm_push_root(lp%d)' % i)
        read_sets = [set() for i in range(N_THREADS)]
        write_sets = [{} for i in range(N_THREADS)]
        active_transactions = set()
        need_abort = set()

        ex.do('stm_stop_transaction()')
        for i in range(N_OBJECTS-1, -1, -1):
            ex.do('lp%d = stm_pop_root()' % i)

        stop_count = 1
        current_thread = 0

        def aborted():
            active_transactions.remove(n_thread)
            write_sets[n_thread].clear()
            read_sets[n_thread].clear()
            need_abort.discard(n_thread)

        remaining_steps = 200
        while remaining_steps > 0 or active_transactions:
            remaining_steps -= 1
            n_thread = rnd.randrange(0, N_THREADS)
            if n_thread != current_thread:
                ex.do('#')
                current_thread = n_thread
                if n_thread in need_abort:
                    ex.do('py.test.raises(Conflict, self.switch, %d)' % n_thread)
                    aborted()
                    continue
                ex.do('self.switch(%d)' % n_thread)
            if n_thread not in active_transactions:
                if remaining_steps <= 0:
                    continue
                ex.do('stm_start_transaction()')
                active_transactions.add(n_thread)

            action = rnd.randrange(0, 7)
            if action < 6 and remaining_steps > 0:
                is_write = action >= 4
                i = rnd.randrange(0, N_OBJECTS)
                if i in write_sets[n_thread]:
                    expected = write_sets[n_thread][i]
                else:
                    expected = head_state[i]
                ex.do("assert stm_get_char(lp%d) == %r" % (i, expected))
                read_sets[n_thread].add(i)
                #
                if is_write:
                    newval = chr(rnd.randrange(0, 256))
                    write_write_conflict = False
                    for t in range(N_THREADS):
                        if t != n_thread:
                            write_write_conflict |= i in write_sets[t]
                    if write_write_conflict:
                        ex.do('py.test.raises(Conflict, stm_set_char, lp%d, %r)'
                              % (i, newval))
                        aborted()
                        continue
                    else:
                        ex.do('stm_set_char(lp%d, %r)' % (i, newval))
                    write_sets[n_thread][i] = newval
            else:
                active_transactions.remove(n_thread)
                changes = []
                modified = sorted(write_sets[n_thread])
                for i in modified:
                    nval = write_sets[n_thread][i]
                    changes.append('lp%d=%r' % (i, nval))
                    head_state[i] = nval
                write_sets[n_thread].clear()
                read_sets[n_thread].clear()
                ex.do('stm_stop_transaction() #%d %s' % (stop_count, ' '.join(changes)))
                stop_count += 1

                for t in range(N_THREADS):
                    if t != n_thread:
                        for i in modified:
                            if i in read_sets[t]:
                                need_abort.add(t)

    def _make_fun(seed):
        def test_fun(self):
            self.test_fixed_16_bytes_objects(seed)
        test_fun.__name__ = 'test_fixed_16_bytes_objects_%d' % seed
        return test_fun

    for _seed in range(5000, 5100):
        _fn = _make_fun(_seed)
        locals()[_fn.__name__] = _fn
