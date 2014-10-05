from support import *
import py, os, struct

udir = py.path.local.make_numbered_dir(prefix = 'stmgc-')


def read_log(filename):
    f = open(filename, 'rb')
    header = f.read(16)
    assert header == "STMGC-C7-PROF01\n"
    result = []
    while True:
        packet = f.read(19)
        if not packet: break
        sec, nsec, threadnum, otherthreadnum, event, len0, len1 = \
              struct.unpack("IIIIBBB", packet)
        result.append((sec + 0.000000001 * nsec,
                       (threadnum, otherthreadnum),
                       event,
                       f.read(len0),
                       f.read(len1)))
    f.close()
    return result


class TestProf(BaseTest):

    def test_simple(self):
        filename = os.path.join(str(udir), 'simple.prof')
        r = lib.stm_set_timing_log(filename, ffi.NULL)
        assert r == 0
        try:
            self.start_transaction()
            self.commit_transaction()
        finally:
            lib.stm_set_timing_log(ffi.NULL, ffi.NULL)

        result = read_log(filename)
        assert result[0][2] == lib.STM_TRANSACTION_START
        assert result[1][2] == lib.STM_GC_MINOR_START
        assert result[2][2] == lib.STM_GC_MINOR_DONE
        assert result[3][2] == lib.STM_TRANSACTION_COMMIT
        assert len(result) == 4

    def test_contention(self):
        @ffi.callback("int(stm_loc_marker_t *, char *, int)")
        def expand_marker(marker, p, s):
            p[0] = chr(100 + marker.odd_number)
            return 1
        filename = os.path.join(str(udir), 'contention.prof')
        r = lib.stm_set_timing_log(filename, expand_marker)
        assert r == 0
        try:
            p = stm_allocate_old(16)
            self.start_transaction()
            assert stm_get_char(p) == '\x00'    # read
            #
            self.switch(1)
            self.start_transaction()
            self.push_root(ffi.cast("object_t *", 19))
            self.push_root(ffi.cast("object_t *", ffi.NULL))
            stm_set_char(p, 'B')                # write
            py.test.raises(Conflict, self.commit_transaction)
        finally:
            lib.stm_set_timing_log(ffi.NULL, ffi.NULL)

        result = read_log(filename)
        id0 = result[0][1][0]
        id1 = result[1][1][0]
        assert result[0][1:5] == ((id0, 0), lib.STM_TRANSACTION_START, '', '')
        assert result[1][1:5] == ((id1, 0), lib.STM_TRANSACTION_START, '', '')
        assert result[2][1:5] == ((id1, 0), lib.STM_GC_MINOR_START, '', '')
        assert result[3][1:5] == ((id1, 0), lib.STM_GC_MINOR_DONE, '', '')
        assert result[4][1:5] == ((id1, id0), lib.STM_CONTENTION_WRITE_READ,
                                  chr(119), '')
        assert result[5][1:5] == ((id1, 0), lib.STM_TRANSACTION_ABORT, '', '')
        assert len(result) == 6
