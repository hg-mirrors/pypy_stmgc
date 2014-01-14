from support import *
import sys, random


class TestLargeMalloc(object):

    def setup_method(self, meth):
        size = 1024 * 1024     # 1MB
        self.rawmem = ffi.new("char[]", size)
        self.size = size
        lib.memset(self.rawmem, 0xcd, size)
        lib.stm_largemalloc_init(self.rawmem, size)

    def test_simple(self):
        d1 = lib.stm_large_malloc(7000)
        d2 = lib.stm_large_malloc(8000)
        assert d2 - d1 == 7016
        d3 = lib.stm_large_malloc(9000)
        assert d3 - d2 == 8016
        #
        lib.stm_large_free(d1)
        lib.stm_large_free(d2)
        #
        d4 = lib.stm_large_malloc(600)
        assert d4 == d1
        d5 = lib.stm_large_malloc(600)
        assert d5 == d4 + 616
        #
        lib.stm_large_free(d5)
        #
        d6 = lib.stm_large_malloc(600)
        assert d6 == d5
        #
        lib.stm_large_free(d4)
        #
        d7 = lib.stm_large_malloc(608)
        assert d7 == d6 + 616
        d8 = lib.stm_large_malloc(600)
        assert d8 == d4
        #
        lib._stm_large_dump()

    def test_overflow_1(self):
        d = lib.stm_large_malloc(self.size - 32)
        assert d == self.rawmem + 16
        lib._stm_large_dump()

    def test_overflow_2(self):
        d = lib.stm_large_malloc(self.size - 16)
        assert d == ffi.NULL
        lib._stm_large_dump()

    def test_overflow_3(self):
        d = lib.stm_large_malloc(sys.maxint & ~7)
        assert d == ffi.NULL
        lib._stm_large_dump()

    def test_resize_arena_reduce_1(self):
        r = lib.stm_largemalloc_resize_arena(self.size - 32)
        assert r == 1
        d = lib.stm_large_malloc(self.size - 32)
        assert d == ffi.NULL
        lib._stm_large_dump()

    def test_resize_arena_reduce_2(self):
        lib.stm_large_malloc(self.size // 2 - 64)
        r = lib.stm_largemalloc_resize_arena(self.size // 2)
        assert r == 1
        lib._stm_large_dump()

    def test_resize_arena_reduce_3(self):
        d1 = lib.stm_large_malloc(128)
        r = lib.stm_largemalloc_resize_arena(self.size // 2)
        assert r == 1
        d2 = lib.stm_large_malloc(128)
        assert d1 == self.rawmem + 16
        assert d2 == d1 + 128 + 16
        lib._stm_large_dump()

    def test_resize_arena_cannot_reduce_1(self):
        lib.stm_large_malloc(self.size // 2)
        r = lib.stm_largemalloc_resize_arena(self.size // 2)
        assert r == 0
        lib._stm_large_dump()

    def test_resize_arena_cannot_reduce_2(self):
        lib.stm_large_malloc(self.size // 2 - 56)
        r = lib.stm_largemalloc_resize_arena(self.size // 2)
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
                assert d[0] == content1
                assert d[length - 1] == content2
                lib.stm_large_free(d)
            else:
                sz = r.randrange(8, 160) * 8
                d = lib.stm_large_malloc(sz)
                print 'alloc %5d  (%s)' % (sz, d)
                assert d != ffi.NULL
                lib.memset(d, 0xdd, sz)
                content1 = chr(r.randrange(0, 256))
                content2 = chr(r.randrange(0, 256))
                d[0] = content1
                d[sz - 1] = content2
                p.append((d, sz, content1, content2))
        lib._stm_large_dump()