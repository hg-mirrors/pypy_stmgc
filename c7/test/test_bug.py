from support import *
import py


class TestBug(BaseTest):

    def test_write_marker_no_conflict(self):
        # initialization
        stm_start_transaction()
        lp0 = stm_allocate(16)
        stm_set_char(lp0, '\x00')
        stm_push_root(lp0)
        lp1 = stm_allocate(16)
        stm_set_char(lp1, '\x01')
        stm_push_root(lp1)
        lp2 = stm_allocate(16)
        stm_set_char(lp2, '\x02')
        stm_push_root(lp2)
        lp3 = stm_allocate(16)
        stm_set_char(lp3, '\x03')
        stm_push_root(lp3)
        lp4 = stm_allocate(16)
        stm_set_char(lp4, '\x04')
        stm_push_root(lp4)
        stm_stop_transaction()
        lp4 = stm_pop_root()
        lp3 = stm_pop_root()
        lp2 = stm_pop_root()
        lp1 = stm_pop_root()
        lp0 = stm_pop_root()
        #
        self.switch(1)
        stm_start_transaction()
        assert stm_get_char(lp1) == '\x01'
        stm_set_char(lp1, '\x15')
        #
        self.switch(0)
        stm_start_transaction()
        assert stm_get_char(lp2) == '\x02'
        #
        self.switch(1)
        assert stm_get_char(lp4) == '\x04'
        assert stm_get_char(lp4) == '\x04'
        assert stm_get_char(lp2) == '\x02'
        assert stm_get_char(lp0) == '\x00'
        assert stm_get_char(lp1) == '\x15'
        assert stm_get_char(lp2) == '\x02'
        stm_stop_transaction() #1 lp1='\x15'
        stm_start_transaction()
        stm_stop_transaction() #2 
        stm_start_transaction()
        assert stm_get_char(lp2) == '\x02'
        assert stm_get_char(lp4) == '\x04'
        assert stm_get_char(lp2) == '\x02'
        assert stm_get_char(lp0) == '\x00'
        assert stm_get_char(lp0) == '\x00'
        assert stm_get_char(lp1) == '\x15'
        assert stm_get_char(lp0) == '\x00'
        assert stm_get_char(lp2) == '\x02'
        assert stm_get_char(lp0) == '\x00'
        assert stm_get_char(lp4) == '\x04'
        stm_set_char(lp4, '\xdf')
        #
        self.switch(0)
        assert stm_get_char(lp3) == '\x03'
        stm_stop_transaction() #3 
        #
        self.switch(1)
        assert stm_get_char(lp4) == '\xdf'
        stm_set_char(lp4, '\x0c')
        assert stm_get_char(lp2) == '\x02'
        assert stm_get_char(lp3) == '\x03'
        #
        self.switch(0)
        stm_start_transaction()
        assert stm_get_char(lp3) == '\x03'
        stm_stop_transaction() #4 
        #
        self.switch(1)
        assert stm_get_char(lp0) == '\x00'
        stm_set_char(lp0, 's')
        #
        self.switch(0)
        stm_start_transaction()
        assert stm_get_char(lp1) == '\x15'
        #
        self.switch(1)
        assert stm_get_char(lp4) == '\x0c'
        stm_set_char(lp4, 'Q')
        assert stm_get_char(lp2) == '\x02'
        #
        self.switch(0)
        assert stm_get_char(lp3) == '\x03'
        #
        self.switch(1)
        assert stm_get_char(lp0) == 's'
        #
        self.switch(0)
        assert stm_get_char(lp4) == '\x04'
        assert stm_get_char(lp1) == '\x15'
        stm_set_char(lp1, '\xd1')
        stm_stop_transaction() #5 lp1='\xd1'
        stm_start_transaction()
        assert stm_get_char(lp2) == '\x02'
        stm_set_char(lp2, 'j')
        #
        py.test.raises(Conflict, self.switch, 1)
        stm_start_transaction()
        assert stm_get_char(lp3) == '\x03'
        #
        self.switch(0)
        assert stm_get_char(lp4) == '\x04'
        #
        self.switch(1)
        assert stm_get_char(lp0) == '\x00'
        assert stm_get_char(lp3) == '\x03'
        #
        self.switch(0)
        assert stm_get_char(lp4) == '\x04'
        assert stm_get_char(lp4) == '\x04'
        #
        self.switch(1)
        assert stm_get_char(lp0) == '\x00'
        #
        self.switch(0)
        assert stm_get_char(lp0) == '\x00'
        assert stm_get_char(lp1) == '\xd1'
        #
        self.switch(1)
        assert stm_get_char(lp3) == '\x03'
        #
        self.switch(0)
        assert stm_get_char(lp1) == '\xd1'
        assert stm_get_char(lp0) == '\x00'
        #
        self.switch(1)
        assert stm_get_char(lp2) == '\x02'
        assert stm_get_char(lp2) == '\x02'
        assert stm_get_char(lp0) == '\x00'
        stm_set_char(lp0, '\xdf')
        #
        self.switch(0)
        assert stm_get_char(lp2) == 'j'
        stm_set_char(lp2, '\xed')
        assert stm_get_char(lp1) == '\xd1'
        #
        self.switch(1)
        assert stm_get_char(lp1) == '\xd1'
        assert stm_get_char(lp3) == '\x03'
        #
        self.switch(0)
        assert stm_get_char(lp2) == '\xed'
        stm_set_char(lp2, '\x02')
        assert stm_get_char(lp2) == '\x02'
        stm_set_char(lp2, 'Q')
        #
        self.switch(1)
        assert stm_get_char(lp0) == '\xdf'
        stm_set_char(lp0, '#')
        #
        self.switch(0)
        assert stm_get_char(lp1) == '\xd1'
        stm_stop_transaction() #6 lp2='Q'
        #
        py.test.raises(Conflict, self.switch, 1)
        stm_start_transaction()
        assert stm_get_char(lp0) == '\x00'
        assert stm_get_char(lp3) == '\x03'
        stm_set_char(lp3, '\xf9')
        #
        self.switch(0)
        stm_start_transaction()
        assert stm_get_char(lp0) == '\x00'
        assert stm_get_char(lp1) == '\xd1'
        #
        self.switch(1)
        stm_stop_transaction() #7 lp3='\xf9'
        #
        self.switch(0)
        stm_stop_transaction() #8 
        stm_start_transaction()
        assert stm_get_char(lp4) == '\x04'
        assert stm_get_char(lp3) == '\xf9'
        #
        self.switch(1)
        stm_start_transaction()
        assert stm_get_char(lp0) == '\x00'
        stm_set_char(lp0, 'N')
        #
        self.switch(0)
        assert stm_get_char(lp4) == '\x04'
        stm_set_char(lp4, 'K')
        #
        self.switch(1)
        assert stm_get_char(lp4) == '\x04'
        assert stm_get_char(lp3) == '\xf9'
        #
        self.switch(0)
        assert stm_get_char(lp3) == '\xf9'
        assert stm_get_char(lp4) == 'K'
        stm_set_char(lp4, '\xce')
        #
        self.switch(1)
        stm_stop_transaction() #9 lp0='N'
        stm_start_transaction()
        assert stm_get_char(lp2) == 'Q'
        assert stm_get_char(lp4) == '\x04'
        assert stm_get_char(lp1) == '\xd1'
        stm_set_char(lp1, '\xdb')
        stm_stop_transaction() #10 lp1='\xdb'
        #
        self.switch(0)
        stm_stop_transaction() #11 lp4='\xce'
        stm_start_transaction()
        assert stm_get_char(lp2) == 'Q'
        assert stm_get_char(lp0) == 'N'
        #
        self.switch(1)
        stm_start_transaction()
        assert stm_get_char(lp0) == 'N'
        stm_set_char(lp0, '\x80')
        #
        stm_stop_transaction()
        py.test.raises(Conflict, self.switch, 0)
