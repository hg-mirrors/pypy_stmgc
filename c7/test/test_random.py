from support import *
import sys, random


class TestRandom(BaseTest):

    def test_fixed_16_bytes_objects(self):
        rnd = random.Random(1010)

        N_OBJECTS = 10
        N_THREADS = 3
        print >> sys.stderr, 'stm_start_transaction()'
        stm_start_transaction()
        plist = [stm_allocate(16) for i in range(N_OBJECTS)]
        read_sets = [{} for i in range(N_THREADS)]
        write_sets = [{} for i in range(N_THREADS)]
        active_transactions = {}

        for i in range(N_OBJECTS):
            print >> sys.stderr, 'p%d = stm_allocate(16)' % i
        for i in range(N_OBJECTS):
            print >> sys.stderr, 'p%d[8] = %r' % (i, chr(i))
            plist[i][8] = chr(i)
        head_state = [[chr(i) for i in range(N_OBJECTS)]]
        commit_log = []
        print >> sys.stderr, 'stm_stop_transaction(False)'
        stm_stop_transaction(False)

        for i in range(N_THREADS):
            print >> sys.stderr, 'self.switch(%d)' % i
            self.switch(i)
        stop_count = 1

        for i in range(10000):
            n_thread = rnd.randrange(0, N_THREADS)
            print >> sys.stderr, '#\nself.switch(%d)' % n_thread
            self.switch(n_thread)
            if n_thread not in active_transactions:
                print >> sys.stderr, 'stm_start_transaction()'
                stm_start_transaction()
                active_transactions[n_thread] = len(commit_log)

            action = rnd.randrange(0, 7)
            if action < 6:
                is_write = action >= 4
                i = rnd.randrange(0, N_OBJECTS)
                print >> sys.stderr, "stm_read(p%d)" % i
                stm_read(plist[i])
                got = plist[i][8]
                print >> sys.stderr, "assert p%d[8] ==" % i,
                my_head_state = head_state[active_transactions[n_thread]]
                prev = read_sets[n_thread].setdefault(i, my_head_state[i])
                print >> sys.stderr, "%r" % (prev,)
                assert got == prev
                #
                if is_write:
                    print >> sys.stderr, 'stm_write(p%d)' % i
                    stm_write(plist[i])
                    newval = chr(rnd.randrange(0, 256))
                    print >> sys.stderr, 'p%d[8] = %r' % (i, newval)
                    plist[i][8] = newval
                    read_sets[n_thread][i] = write_sets[n_thread][i] = newval
            else:
                src_index = active_transactions.pop(n_thread)
                conflict = False
                for i in range(src_index, len(commit_log)):
                    for j in commit_log[i]:
                        if j in read_sets[n_thread]:
                            conflict = True
                print >> sys.stderr, "stm_stop_transaction(%r) #%d" % (
                    conflict, stop_count)
                stop_count += 1
                stm_stop_transaction(conflict)
                #
                if not conflict:
                    hs = head_state[-1][:]
                    for i, newval in write_sets[n_thread].items():
                        hs[i] = newval
                        assert plist[i][8] == newval
                    head_state.append(hs)
                    commit_log.append(write_sets[n_thread].keys())
                    print >> sys.stderr, '#', head_state[-1]
                    print >> sys.stderr, '# log:', commit_log[-1]
                write_sets[n_thread].clear()
                read_sets[n_thread].clear()
