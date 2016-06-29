from support import *
import py



class TestNoConflict(BaseTest):

    def test_basic(self):
        self.start_transaction()
        o = stm_allocate_noconflict(16)
        stm_set_char(o, 'x')
        self.push_root(o)
        self.commit_transaction()
        o = self.pop_root()
        self.push_root(o)

        self.switch(1)

        self.start_transaction()
        assert stm_get_char(o) == 'x'

    def test_propagate_on_validation(self):
        self.start_transaction()
        o = stm_allocate_noconflict(16)
        self.push_root(o)
        self.commit_transaction()

        self.start_transaction()
        o = self.pop_root()
        self.push_root(o)

        self.switch(1)

        self.start_transaction()
        assert stm_get_char(o) == '\0'

        self.switch(0)
        stm_set_char(o, 'a')
        self.commit_transaction()

        self.switch(1, False)
        assert stm_get_char(o) == '\0'
        stm_validate()
        assert stm_get_char(o) == 'a'

    def test_propagate_on_validation2(self):
        self.start_transaction()
        o = stm_allocate_noconflict(16)
        self.push_root(o)
        self.commit_transaction()

        self.start_transaction()
        o = self.pop_root()
        self.push_root(o)

        self.switch(1)

        self.start_transaction()
        assert stm_get_char(o) == '\0'
        stm_set_char(o, 'b') # later lost

        self.switch(0)
        stm_set_char(o, 'a')
        self.commit_transaction()

        self.switch(1, False)
        assert stm_get_char(o) == 'b'
        stm_validate()
        assert stm_get_char(o) == 'a'

    def test_abort_doesnt_revert(self):
        self.start_transaction()
        o = stm_allocate_noconflict(16)
        self.push_root(o)
        self.commit_transaction()

        self.start_transaction()
        o = self.pop_root()
        self.push_root(o)

        self.switch(1)

        self.start_transaction()
        assert stm_get_char(o) == '\0'
        stm_set_char(o, 'b') # later lost

        self.switch(0)
        stm_set_char(o, 'a')
        self.commit_transaction()

        self.switch(1, False)
        assert stm_get_char(o) == 'b'
        stm_validate()
        assert stm_get_char(o) == 'a'
        # now make sure we never revert back to '\0'
        # since then we would need to trace backup copies
        # in the GC to keep refs alive there
        self.abort_transaction()
        self.start_transaction()
        assert stm_get_char(o) == 'a'


    def test_huge_obj(self):
        self.start_transaction()
        o = stm_allocate_noconflict(1000+20*CARD_SIZE)
        self.push_root(o)
        self.commit_transaction()
        self.start_transaction()
        o = self.pop_root()
        self.push_root(o)

        stm_set_char(o, 'x', HDR, True)
        stm_set_char(o, 'y', 1000, True)

        self.switch(1)
        self.start_transaction()
        assert stm_get_char(o, HDR) == '\0'
        stm_set_char(o, 'b', HDR, False) # later lost

        self.switch(0)
        self.commit_transaction()

        self.switch(1, False)
        assert stm_get_char(o, HDR) == 'b'
        assert stm_get_char(o, 1000) == '\0'
        stm_validate()
        assert stm_get_char(o, HDR) == 'x'
        assert stm_get_char(o, 1000) == 'y'
        self.abort_transaction()
        self.start_transaction()
        assert stm_get_char(o, HDR) == 'x'
        assert stm_get_char(o, 1000) == 'y'

    def test_only_read(self):
        self.start_transaction()
        o = stm_allocate_noconflict(16)
        self.push_root(o)
        self.commit_transaction()

        self.start_transaction()
        o = self.pop_root()
        self.push_root(o)

        self.switch(1)

        self.start_transaction()
        stm_read(o) # don't touch the memory

        self.switch(0)
        stm_set_char(o, 'a')
        self.commit_transaction()

        self.switch(1, False)
        stm_validate()
        assert stm_get_char(o) == 'a'

    def test_obj_reset_on_validate_like_it_was_never_written_to(self):
        get_card_value = lib._stm_get_card_value

        # if noconfl objs are reset during stm_validate(), their WB flag gets
        # lost. make sure it is not in any of the lists where the WB flag is
        # required / expected.
        self.start_transaction()
        o = stm_allocate_noconflict(16)
        oh = stm_allocate_noconflict(1000+20*CARD_SIZE)
        self.push_root(o)
        self.push_root(oh)
        self.commit_transaction()

        self.start_transaction()
        oh = self.pop_root()
        o = self.pop_root()
        self.push_root(o)
        self.push_root(oh)

        stm_set_char(o, 'a')
        stm_set_char(oh, 'x', use_cards=True)
        assert o in modified_old_objects()
        assert oh in modified_old_objects()
        assert o in objects_pointing_to_nursery()
        assert oh not in objects_pointing_to_nursery()
        assert get_card_value(oh, 0) == CARD_MARKED
        assert oh in old_objects_with_cards_set()

        self.switch(1)
        self.start_transaction()
        stm_set_char(o, 'b')
        stm_set_char(oh, 'y', use_cards=True)
        self.commit_transaction()

        self.switch(0, False)
        assert stm_get_char(o) == 'a'
        assert stm_get_char(oh) == 'x'
        assert o in modified_old_objects()
        assert oh in modified_old_objects()
        assert o in objects_pointing_to_nursery()
        assert oh not in objects_pointing_to_nursery()
        assert oh in old_objects_with_cards_set()
        stm_validate()
        assert stm_get_char(o) == 'b'
        assert stm_get_char(oh) == 'y'
        assert o not in modified_old_objects()
        assert oh not in modified_old_objects()
        assert o not in objects_pointing_to_nursery()
        assert oh not in objects_pointing_to_nursery()
        assert get_card_value(oh, 0) == CARD_CLEAR
        assert oh not in old_objects_with_cards_set()
        stm_minor_collect()

    def test_reset_obj_during_commit_process(self):
        self.start_transaction()
        o = stm_allocate_noconflict(16)
        self.push_root(o)
        self.commit_transaction()
        assert count_commit_log_entries() == 1

        self.start_transaction()
        o = self.pop_root()
        self.push_root(o)
        stm_set_char(o, 'a')

        self.switch(1)
        self.start_transaction()
        stm_set_char(o, 'b')
        self.commit_transaction()

        assert count_commit_log_entries() == 2
        assert o in last_commit_log_entry_objs()

        self.switch(0, False)
        assert stm_get_char(o) == 'a'
        self.commit_transaction() # validates and succeeds!

        # the changes we did to our noconfl obj obviously must not appear in
        # the commit log (since they get reverted by the validation step during
        # commit). However, there was a bug that constructed the list of
        # changes only once and does not account for validation to remove
        # changes from that list.
        assert count_commit_log_entries() == 3
        assert o not in last_commit_log_entry_objs()
