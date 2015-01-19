from support import *
import random


def pageof(p):
    return int(ffi.cast("uintptr_t", p)) >> 12


class TestSmallMalloc(BaseTest):

    def setup_method(self, method):
        BaseTest.setup_method(self, method)
        @ffi.callback("bool(char *)")
        def keep(data):
            p = ffi.cast("object_t *", data)
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

    def test_sweep_trivial(self):
        lib._stm_smallmalloc_sweep_test()

    def test_sweep_freeing_simple(self):
        p1 = stm_allocate_old_small(16)
        self.has_been_asked_for = []
        lib._stm_smallmalloc_sweep_test()
        assert p1 in self.has_been_asked_for

    def test_sweep_freeing_random_subset(self):
        for i in range(50):
            # allocate a page's worth of objs
            page0 = [stm_allocate_old_small(16) for i in range(0, 4096, 16)]
            assert len(set(map(pageof, page0))) == 1, "all in the same page"
            tid = lib._get_type_id(page0[0])
            assert tid == 58, "current way to do it"

            # repeatedly free a subset until no objs are left in that page
            while len(page0) > 0:
                # keep half of them around
                self.keep_me = set(random.sample(page0, len(page0) // 2))
                self.has_been_asked_for = []
                lib._stm_smallmalloc_sweep_test()

                print len(page0), len(self.has_been_asked_for)
                assert sorted(page0) == self.has_been_asked_for, "all objs were observed"

                # get list of objs that were not freed
                page0remaining = []
                for p in page0:
                    if p in self.keep_me:
                        assert lib._get_type_id(p) == tid
                        page0remaining.append(p)
                    elif len(self.keep_me) > 0: # otherwise page not accessible from seg1
                        assert lib._get_type_id(p) != tid, "should have garbage there now (0xdd)"
                page0 = page0remaining

                if len(page0) > 10:
                    # allocate one obj for noise if we do another iteration anyway
                    p = stm_allocate_old_small(16)
                    assert pageof(p) == pageof(page0[0])
                    page0.append(p)

    def test_sweep_full_page_remains_full(self):
        page0 = [stm_allocate_old_small(16) for i in range(0, 4096, 16)]
        tid = lib._get_type_id(page0[0])
        self.keep_me = set(page0)
        lib._stm_smallmalloc_sweep_test()
        for p in page0:
            assert lib._get_type_id(p) == tid
