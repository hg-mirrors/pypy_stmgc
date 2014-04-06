from support import *
import sys, random

ra = lambda x: x   # backward compat.

class TestLargeMalloc(BaseTest):
    def setup_method(self, meth):
        # initialize some big heap in stm_setup()
        BaseTest.setup_method(self, meth)

        # now re-initialize the heap to 1MB with 0xcd in it
        self.size = 1024 * 1024     # 1MB
        self.rawmem = lib._stm_largemalloc_data_start()

        lib.memset(self.rawmem, 0xcd, self.size)
        lib._stm_largemalloc_init_arena(self.rawmem, self.size)
        lib._stm_mutex_pages_lock()   # for this file

    def test_simple(self):
        #
        lib._stm_large_dump()
        d1 = lib._stm_large_malloc(7000)
        lib._stm_large_dump()
        d2 = lib._stm_large_malloc(8000)
        print d1
        print d2
        assert ra(d2) - ra(d1) == 7016
        d3 = lib._stm_large_malloc(9000)
        assert ra(d3) - ra(d2) == 8016
        #
        lib._stm_large_free(d1)
        lib._stm_large_free(d2)
        #
        d4 = lib._stm_large_malloc(600)
        assert d4 == d1
        d5 = lib._stm_large_malloc(600)
        assert ra(d5) == ra(d4) + 616
        #
        lib._stm_large_free(d5)
        #
        d6 = lib._stm_large_malloc(600)
        assert d6 == d5
        #
        lib._stm_large_free(d4)
        #
        d7 = lib._stm_large_malloc(608)
        assert ra(d7) == ra(d6) + 616
        d8 = lib._stm_large_malloc(600)
        assert d8 == d4
        #
        lib._stm_large_dump()

    def test_overflow_1(self):
        d = lib._stm_large_malloc(self.size - 32)
        assert ra(d) == self.rawmem + 16
        lib._stm_large_dump()

    def test_overflow_2(self):
        d = lib._stm_large_malloc(self.size - 16)
        assert d == ffi.NULL
        lib._stm_large_dump()

    def test_overflow_3(self):
        d = lib._stm_large_malloc(sys.maxint & ~7)
        assert d == ffi.NULL
        lib._stm_large_dump()

    def test_resize_arena_reduce_1(self):
        r = lib._stm_largemalloc_resize_arena(self.size - 32)
        assert r == 1
        d = lib._stm_large_malloc(self.size - 32)
        assert d == ffi.NULL
        lib._stm_large_dump()

    def test_resize_arena_reduce_2(self):
        lib._stm_large_malloc(self.size // 2 - 80)
        r = lib._stm_largemalloc_resize_arena(self.size // 2)
        assert r == 1
        lib._stm_large_dump()

    def test_resize_arena_reduce_3(self):
        d1 = lib._stm_large_malloc(128)
        r = lib._stm_largemalloc_resize_arena(self.size // 2)
        assert r == 1
        d2 = lib._stm_large_malloc(128)
        assert ra(d1) == self.rawmem + 16
        assert ra(d2) == ra(d1) + 128 + 16
        lib._stm_large_dump()

    def test_resize_arena_cannot_reduce_1(self):
        lib._stm_large_malloc(self.size // 2)
        r = lib._stm_largemalloc_resize_arena(self.size // 2)
        assert r == 0
        lib._stm_large_dump()

    def test_resize_arena_cannot_reduce_2(self):
        lib._stm_large_malloc(self.size // 2 - 56)
        r = lib._stm_largemalloc_resize_arena(self.size // 2)
        assert r == 0
        lib._stm_large_dump()

    def test_random(self):
        r = random.Random(1007)
        p = []
        for i in range(100000):
            if len(p) != 0 and (len(p) > 100 or r.randrange(0, 5) < 2):
                index = r.randrange(0, len(p))
                d, length, content1, content2 = p.pop(index)
                print ' free %5d  (%s)' % (length, d)
                assert ra(d)[0] == content1
                assert ra(d)[length - 1] == content2
                lib._stm_large_free(d)
            else:
                sz = r.randrange(8, 160) * 8
                d = lib._stm_large_malloc(sz)
                print 'alloc %5d  (%s)' % (sz, d)
                assert d != ffi.NULL
                lib.memset(ra(d), 0xdd, sz)
                content1 = chr(r.randrange(0, 256))
                content2 = chr(r.randrange(0, 256))
                ra(d)[0] = content1
                ra(d)[sz - 1] = content2
                p.append((d, sz, content1, content2))
        lib._stm_large_dump()

    def test_random_largemalloc_sweep(self, constrained_size_range=False):
        @ffi.callback("bool(char *)")
        def keep(data):
            try:
                if data in from_before:
                    return False
                index = all.index(data)
                seen_for.add(index)
                return index in keep_me
            except Exception, e:
                errors.append(e)
                raise
        lib._stm_largemalloc_keep = keep
        errors = []
        from_before = set()

        r = random.Random(1000)
        for j in range(500):
            if constrained_size_range:
                max = 120
            else:
                max = 500
            sizes = [random.choice(range(104, max, 8)) for i in range(20)]
            all = [lib._stm_large_malloc(size) for size in sizes]
            print all

            for i in range(len(all)):
                all[i][50] = chr(65 + i)
            all_orig = all[:]

            keep_me = set()
            for i in range(len(all)):
                if r.random() < 0.5:
                    print 'free:', all[i]
                    lib._stm_large_free(all[i])
                    all[i] = None
                elif r.random() < 0.5:
                    keep_me.add(i)

            seen_for = set()
            lib._stm_largemalloc_sweep()
            if errors:
                raise errors[0]
            assert seen_for == set([i for i in range(len(all))
                                      if all[i] is not None])
            lib._stm_large_dump()

            from_before = [all[i] for i in keep_me]

            for i in range(len(all)):
                if i in keep_me:
                    assert all[i][50] == chr(65 + i)
                else:
                    assert all_orig[i][50] == '\xDE'

    def test_random_largemalloc_sweep_constrained_size_range(self):
        self.test_random_largemalloc_sweep(constrained_size_range=True)
