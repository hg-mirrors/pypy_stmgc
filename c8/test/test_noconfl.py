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
