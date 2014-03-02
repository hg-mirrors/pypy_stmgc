from support import *
import py
import weakref


prebuilt_dict = weakref.WeakKeyDictionary()

def _prebuilt(size, tid):
    assert size >= 16
    assert (size & 7) == 0
    myobj1 = ffi.new("char[]", size)
    myobj = ffi.cast("object_t *", myobj1)
    prebuilt_dict[myobj] = myobj1
    ffi.cast("uint32_t *", myobj)[1] = tid
    return myobj

def prebuilt(size):
    return _prebuilt(size, 42 + size)

def prebuilt_refs(n):
    return _prebuilt(HDR + n * WORD, 421420 + n)


class TestPrebuilt(BaseTest):

    def test_simple_prebuilt(self):
        static1 = prebuilt(16)
        ffi.cast("char *", static1)[8:11] = 'ABC'
        print static1
        lp = lib.stm_setup_prebuilt(static1)
        #
        self.start_transaction()
        assert stm_get_char(lp, 8) == 'A'
        assert stm_get_char(lp, 9) == 'B'
        assert stm_get_char(lp, 10) == 'C'

    def test_prebuilt_rec(self):
        static1 = prebuilt_refs(2)
        static2 = prebuilt(16)
        ffi.cast("char *", static2)[8:11] = 'ABC'
        ffi.cast("object_t **", static1)[1] = static2
        lp1 = lib.stm_setup_prebuilt(static1)
        #
        self.start_transaction()
        assert not stm_get_ref(lp1, 1)
        lp2 = stm_get_ref(lp1, 0)
        print lp2
        assert stm_get_char(lp2, 8) == 'A'
        assert stm_get_char(lp2, 9) == 'B'
        assert stm_get_char(lp2, 10) == 'C'

    def test_prebuilt_rec_cycle(self):
        static1 = prebuilt_refs(1)
        static2 = prebuilt_refs(1)
        ffi.cast("object_t **", static1)[1] = static2
        ffi.cast("object_t **", static2)[1] = static1
        lp1 = lib.stm_setup_prebuilt(static1)
        #
        self.start_transaction()
        lp2 = stm_get_ref(lp1, 0)
        print lp2
        assert lp2 != lp1
        assert stm_get_ref(lp2, 0) == lp1
        assert lib._stm_get_flags(lp1) == lib._STM_GCFLAG_WRITE_BARRIER
        assert lib._stm_get_flags(lp2) == lib._STM_GCFLAG_WRITE_BARRIER

    def test_multiple_calls_to_stm_setup_prebuilt_1(self, reverse=False):
        static1 = prebuilt_refs(1)
        static2 = prebuilt_refs(1)
        ffi.cast("object_t **", static1)[1] = static2
        if not reverse:
            lp1 = lib.stm_setup_prebuilt(static1)
            lp2 = lib.stm_setup_prebuilt(static2)
        else:
            lp2 = lib.stm_setup_prebuilt(static2)
            lp1 = lib.stm_setup_prebuilt(static1)
        #
        self.start_transaction()
        assert stm_get_ref(lp1, 0) == lp2
        assert stm_get_ref(lp2, 0) == ffi.NULL
        assert lib._stm_get_flags(lp1) == lib._STM_GCFLAG_WRITE_BARRIER
        assert lib._stm_get_flags(lp2) == lib._STM_GCFLAG_WRITE_BARRIER

    def test_multiple_calls_to_stm_setup_prebuilt_2(self):
        self.test_multiple_calls_to_stm_setup_prebuilt_1(reverse=True)
