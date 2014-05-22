from support import *
import py


class TestBasic(BaseTest):

    def _collect(self, kind):
        if kind == 0:
            stm_minor_collect()
        elif kind == 1:
            stm_major_collect()
        elif kind == 2:
            self.switch(1)
            self.start_transaction()
            stm_major_collect()
            self.abort_transaction()
            self.switch(0)

    def test_simple(self):
        o = stm_allocate_old(1024, True)
        self.start_transaction()
        stm_read(o)
        stm_write(o)
        self.commit_transaction()

    def test_simple2(self):
        o = stm_allocate_old(1024, True)
        self.start_transaction()
        stm_write_card(o, 5)
        assert not stm_was_written(o) # don't remove GCFLAG_WRITE_BARRIER
        assert stm_was_written_card(o)
        self.commit_transaction()

    @py.test.mark.parametrize("k", range(3))
    def test_overflow(self, k):
        self.start_transaction()
        o = stm_allocate(1024, True)

        self.push_root(o)
        self._collect(k)
        o = self.pop_root()

        stm_write_card(o, 5)

        assert o in old_objects_with_cards()
        assert o not in modified_old_objects() # overflow object
        assert o not in objects_pointing_to_nursery()
        # don't remove GCFLAG_WB
        assert not stm_was_written(o)
        stm_write(o)
        assert stm_was_written(o)
        self.commit_transaction()

    def test_nursery(self):
        o = stm_allocate_old_refs(200, True)
        self.start_transaction()
        p = stm_allocate(64, True)
        d = stm_allocate(64, True)
        stm_set_ref(o, 199, p, True)

        # without a write-barrier:
        lib._set_ptr(o, 0, d)

        self.push_root(o)
        stm_minor_collect()
        o = self.pop_root()

        pn = stm_get_ref(o, 199)
        assert not is_in_nursery(pn)
        assert pn != p

        # d was not traced!
        dn = stm_get_ref(o, 0)
        assert is_in_nursery(dn)
        assert dn == d

        assert not stm_was_written(o)
        stm_write_card(o, 2)
        assert stm_was_written_card(o)

        # card cleared after last collection,
        # so no retrace of index 199:
        d2 = stm_allocate(64, True)
        # without a write-barrier:
        lib._set_ptr(o, 199, d2)
        self.push_root(o)
        stm_minor_collect()
        o = self.pop_root()
        # d2 was not traced!
        dn = stm_get_ref(o, 199)
        assert is_in_nursery(dn)
        assert dn == d2

    def test_nursery2(self):
        o = stm_allocate_old_refs(200, True)
        self.start_transaction()
        p = stm_allocate(64)
        d = stm_allocate(64)
        e = stm_allocate(64)
        stm_set_ref(o, 199, p, True)
        stm_set_ref(o, 1, d, False)
        lib._set_ptr(o, 100, e) # no barrier

        self.push_root(o)
        stm_minor_collect()
        o = self.pop_root()

        # stm_write in stm_set_ref made it trace everything
        assert not is_in_nursery(stm_get_ref(o, 199))
        assert not is_in_nursery(stm_get_ref(o, 1))
        assert not is_in_nursery(stm_get_ref(o, 100))

    def test_nursery3(self):
        o = stm_allocate_old_refs(200, True)
        self.start_transaction()
        stm_minor_collect()

        p = stm_allocate(64)
        d = stm_allocate(64)
        e = stm_allocate(64)
        stm_set_ref(o, 199, p, True)
        stm_set_ref(o, 1, d, True)
        lib._set_ptr(o, 100, e) # no card marked!

        assert not stm_was_written(o)
        assert stm_was_written_card(o)

        self.push_root(o)
        stm_minor_collect()
        o = self.pop_root()

        assert not is_in_nursery(stm_get_ref(o, 199))
        assert not is_in_nursery(stm_get_ref(o, 1))
        assert stm_get_ref(o, 100) == e # not traced

    def test_abort_cleanup(self):
        o = stm_allocate_old_refs(200, True)
        self.start_transaction()
        stm_minor_collect()

        p = stm_allocate_refs(64)
        d = stm_allocate(64)
        e = stm_allocate(64)
        stm_set_ref(o, 199, p, True)
        stm_set_ref(o, 1, d, True)
        stm_set_ref(p, 1, e)

        self.abort_transaction()

        assert not modified_old_objects()
        assert not objects_pointing_to_nursery()
        assert not old_objects_with_cards()

        self.start_transaction()
        d = stm_allocate(64)
        e = stm_allocate(64)
        lib._set_ptr(o, 199, d) # no barrier
        stm_set_ref(o, 1, e, True) # card barrier

        self.push_root(o)
        stm_minor_collect()
        o = self.pop_root()

        assert not is_in_nursery(stm_get_ref(o, 1))
        assert is_in_nursery(stm_get_ref(o, 199)) # not traced

    @py.test.mark.parametrize("k", range(3))
    def test_major_gc(self, k):
        o = stm_allocate_old_refs(200, True)
        self.start_transaction()
        p = stm_allocate(64)
        stm_set_ref(o, 0, p, True)

        self.push_root(o)
        stm_major_collect()
        o = self.pop_root()

        stm_set_ref(o, 1, ffi.NULL, True)
        p = stm_get_ref(o, 0)
        assert stm_was_written_card(o)

        self.push_root(o)
        self._collect(k)
        o = self.pop_root()

        assert not stm_was_written_card(o)
        assert stm_get_ref(o, 0) == p
        self.commit_transaction()

    def test_synchronize_objs(self):
        o = stm_allocate_old(2000, True)

        self.start_transaction()
        stm_set_char(o, 'a', 1000, False)
        self.commit_transaction()

        self.switch(1)

        self.start_transaction()
        stm_set_char(o, 'b', 1001, False)
        assert stm_get_char(o, 1000) == 'a'
        self.commit_transaction()

        self.switch(0)

        self.start_transaction()
        assert stm_get_char(o, 1001) == 'b'

        stm_set_char(o, 'c', 1000, True)
        stm_set_char(o, 'c', 1000+CARD_SIZE, True)
        stm_set_char(o, 'c', 1000+CARD_SIZE*2, True)
        stm_set_char(o, 'c', 1000+CARD_SIZE*3, True)

        stm_set_char(o, 'd', 1000+CARD_SIZE*10, True)

        stm_set_char(o, 'e', 1000+CARD_SIZE*12, True)
        self.commit_transaction()

        self.switch(1)

        self.start_transaction()
        assert stm_get_char(o, 1000) == 'c'
        assert stm_get_char(o, 1000+CARD_SIZE) == 'c'
        assert stm_get_char(o, 1000+CARD_SIZE*2) == 'c'
        assert stm_get_char(o, 1000+CARD_SIZE*3) == 'c'

        assert stm_get_char(o, 1000+CARD_SIZE*10) == 'd'

        assert stm_get_char(o, 1000+CARD_SIZE*12) == 'e'

        self.commit_transaction()
