from support import *


def pageof(p):
    return int(ffi.cast("uintptr_t", p)) >> 12


class TestSmallMalloc(BaseTest):

    def setup_method(self, method):
        BaseTest.setup_method(self, method)
        @ffi.callback("bool(char *)")
        def keep(data):
            return data in self.keep_me
        lib._stm_smallmalloc_keep = keep
        self._keepalive_keep_function = keep
        self.keep_me = set()

    def test_simple_uniform(self):
        page0 = [stm_allocate_old_small(16) for i in range(0, 4096, 16)]
        assert len(set(map(pageof, page0))) == 1
        #
        page1 = [stm_allocate_old_small(16) for i in range(0, 4096, 16)]
        assert len(set(map(pageof, page1))) == 1
        #
        assert len(set(map(pageof, page0 + page1))) == 2

    def test_different_sizes_different_pages(self):
        seen = []
        for i in range(2, GC_N_SMALL_REQUESTS):
            p = pageof(stm_allocate_old_small(8 * i))
            assert p not in seen
            seen.append(p)
        for i in range(2, GC_N_SMALL_REQUESTS):
            p = pageof(stm_allocate_old_small(8 * i))
            assert p == seen[0]
            seen.pop(0)

    def test_sweep_freeing(self):
        p1 = stm_allocate_old_small(16)
        lib._stm_smallmalloc_sweep()
