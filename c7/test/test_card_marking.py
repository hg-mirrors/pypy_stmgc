from support import *
import py

class TestBasic(BaseTest):

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

    def test_overflow(self):
        self.start_transaction()
        o = stm_allocate(1024, True)
        self.push_root(o)
        stm_minor_collect()
        o = self.pop_root()
        stm_write_card(o, 5)
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
        lib._set_ptr(o, 100, e)

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

        print modified_old_objects()
        print objects_pointing_to_nursery()
        print old_objects_with_cards()

        self.push_root(o)
        stm_minor_collect()
        o = self.pop_root()

        assert not is_in_nursery(stm_get_ref(o, 199))
        assert not is_in_nursery(stm_get_ref(o, 1))
        assert stm_get_ref(o, 100) == e # not traced
