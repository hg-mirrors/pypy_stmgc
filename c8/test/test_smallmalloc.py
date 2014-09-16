from support import *
import random


def pageof(p):
    return int(ffi.cast("uintptr_t", p)) >> 12


class TestSmallMalloc(BaseTest):

    def setup_method(self, method):
        BaseTest.setup_method(self, method)
        @ffi.callback("bool(char *)")
        def keep(data):
            p = ffi.cast("object_t *", data - lib.stm_object_pages)
            self.has_been_asked_for.append(p)
            return p in self.keep_me
        lib._stm_smallmalloc_keep = keep
        self._keepalive_keep_function = keep
        self.keep_me = set()
        self.has_been_asked_for = []

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

    def test_sweep_freeing_simple(self):
        p1 = stm_allocate_old_small(16)
        lib._stm_smallmalloc_sweep()

    def test_sweep_freeing_random_subset(self):
        for i in range(50):
            page0 = [stm_allocate_old_small(16) for i in range(0, 4096, 16)]
            assert len(set(map(pageof, page0))) == 1
            tid = lib._get_type_id(page0[0])
            while len(page0) > 0:
                self.keep_me = set(random.sample(page0, len(page0) // 2))
                self.has_been_asked_for = []
                lib._stm_smallmalloc_sweep()
                assert sorted(page0) == self.has_been_asked_for
                page0r = []
                for p in page0:
                    if p in self.keep_me:
                        assert lib._get_type_id(p) == tid
                        page0r.append(p)
                    else:
                        assert lib._get_type_id(p) != tid
                page0 = page0r
                if len(page0) > 10:
                    p = stm_allocate_old_small(16)
                    assert pageof(p) == pageof(page0[0])
                    page0.append(p)

    def test_sweep_full_page_remains_full(self):
        page0 = [stm_allocate_old_small(16) for i in range(0, 4096, 16)]
        tid = lib._get_type_id(page0[0])
        self.keep_me = set(page0)
        lib._stm_smallmalloc_sweep()
        for p in page0:
            assert lib._get_type_id(p) == tid
