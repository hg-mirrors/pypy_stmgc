from support import *


class TestLargeMalloc(object):

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
