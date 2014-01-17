from support import *
import py


class TestBug(BaseTest):

    def test_bug1(self):
        stm_start_transaction()
        p8 = stm_allocate(16)
        p8[8] = '\x08'
        stm_stop_transaction(False)
        #
        self.switch("sub1")
        self.switch("main")
        stm_start_transaction()
        stm_write(p8)
        p8[8] = '\x97'
        #
        self.switch("sub1")
        stm_start_transaction()
        stm_read(p8)
        assert p8[8] == '\x08'

    def test_bug2(self):
        stm_start_transaction()
        p0 = stm_allocate(16)
        p1 = stm_allocate(16)
        p2 = stm_allocate(16)
        p3 = stm_allocate(16)
        p4 = stm_allocate(16)
        p5 = stm_allocate(16)
        p6 = stm_allocate(16)
        p7 = stm_allocate(16)
        p8 = stm_allocate(16)
        p9 = stm_allocate(16)
        p0[8] = '\x00'
        p1[8] = '\x01'
        p2[8] = '\x02'
        p3[8] = '\x03'
        p4[8] = '\x04'
        p5[8] = '\x05'
        p6[8] = '\x06'
        p7[8] = '\x07'
        p8[8] = '\x08'
        p9[8] = '\t'
        stm_stop_transaction(False)
        self.switch(0)
        self.switch(1)
        self.switch(2)
        #
        self.switch(1)
        stm_start_transaction()
        stm_read(p7)
        assert p7[8] == '\x07'
        #
        self.switch(1)
        stm_read(p0)
        assert p0[8] == '\x00'
        #
        self.switch(1)
        stm_read(p4)
        assert p4[8] == '\x04'
        #
        self.switch(0)
        stm_start_transaction()
        stm_read(p3)
        assert p3[8] == '\x03'
        #
        self.switch(2)
        stm_start_transaction()
        stm_read(p8)
        assert p8[8] == '\x08'
        stm_write(p8)
        p8[8] = '\x08'
        #
        self.switch(0)
        stm_read(p0)
        assert p0[8] == '\x00'
        #
        self.switch(0)
        stm_read(p0)
        assert p0[8] == '\x00'
        #
        self.switch(1)
        stm_read(p2)
        assert p2[8] == '\x02'
        #
        self.switch(2)
        stm_read(p2)
        assert p2[8] == '\x02'
        #
        self.switch(2)
        stm_read(p2)
        assert p2[8] == '\x02'
        stm_write(p2)
        p2[8] = 'm'
        #
        self.switch(0)
        stm_read(p4)
        assert p4[8] == '\x04'
        stm_write(p4)
        p4[8] = '\xc5'
        #
        self.switch(2)
        stm_read(p1)
        assert p1[8] == '\x01'
        #
        self.switch(2)
        stm_stop_transaction(False) #1
        # ['\x00', '\x01', 'm', '\x03', '\x04', '\x05', '\x06', '\x07', '\x08', '\t']
        # log: [8, 2]
        #
        self.switch(0)
        stm_stop_transaction(False) #2
        # ['\x00', '\x01', 'm', '\x03', '\xc5', '\x05', '\x06', '\x07', '\x08', '\t']
        # log: [4]
        #
        self.switch(0)
        stm_start_transaction()
        stm_read(p6)
        assert p6[8] == '\x06'
        #
        self.switch(0)
        stm_read(p4)
        assert p4[8] == '\xc5'
        #
        self.switch(0)
        stm_read(p4)
        assert p4[8] == '\xc5'
        #
        self.switch(1)
        stm_read(p0)
        assert p0[8] == '\x00'
        #
        self.switch(1)
        stm_stop_transaction(True) #3
        # conflict: 0xdf0a8028
        #
        self.switch(2)
        stm_start_transaction()
        stm_read(p6)
        assert p6[8] == '\x06'
        #
        self.switch(1)
        stm_start_transaction()
        stm_read(p1)
        assert p1[8] == '\x01'
        #
        self.switch(0)
        stm_read(p4)
        assert p4[8] == '\xc5'
        stm_write(p4)
        p4[8] = '\x0c'
        #
        self.switch(2)
        stm_read(p2)
        assert p2[8] == 'm'
        stm_write(p2)
        p2[8] = '\x81'
        #
        self.switch(2)
        stm_read(p7)
        assert p7[8] == '\x07'
        #
        self.switch(0)
        stm_read(p5)
        assert p5[8] == '\x05'
        stm_write(p5)
        p5[8] = 'Z'
        #
        self.switch(1)
        stm_stop_transaction(False) #4
        # ['\x00', '\x01', 'm', '\x03', '\xc5', '\x05', '\x06', '\x07', '\x08', '\t']
        # log: []
        #
        self.switch(2)
        stm_read(p8)
        assert p8[8] == '\x08'
        #
        self.switch(0)
        stm_read(p0)
        assert p0[8] == '\x00'
        #
        self.switch(1)
        stm_start_transaction()
        stm_read(p0)
        assert p0[8] == '\x00'
        #
        self.switch(2)
        stm_read(p9)
        assert p9[8] == '\t'
        stm_write(p9)
        p9[8] = '\x81'
        #
        self.switch(0)
        stm_read(p0)
        assert p0[8] == '\x00'
        #
        self.switch(1)
        stm_read(p2)
        assert p2[8] == 'm'
        #
        self.switch(2)
        stm_read(p9)
        assert p9[8] == '\x81'
        stm_write(p9)
        p9[8] = 'g'
        #
        self.switch(1)
        stm_read(p3)
        assert p3[8] == '\x03'
        #
        self.switch(2)
        stm_read(p7)
        assert p7[8] == '\x07'
        #
        self.switch(1)
        stm_read(p1)
        assert p1[8] == '\x01'
        #
        self.switch(0)
        stm_read(p2)
        assert p2[8] == 'm'
        stm_write(p2)
        p2[8] = 'T'
        #
        self.switch(2)
        stm_read(p4)
        assert p4[8] == '\xc5'
        #
        self.switch(2)
        stm_read(p9)
        assert p9[8] == 'g'
        #
        self.switch(2)
        stm_read(p1)
        assert p1[8] == '\x01'
        stm_write(p1)
        p1[8] = 'L'
        #
        self.switch(0)
        stm_read(p0)
        assert p0[8] == '\x00'
        #
        self.switch(2)
        stm_read(p0)
        assert p0[8] == '\x00'
        stm_write(p0)
        p0[8] = '\xf3'
        #
        self.switch(1)
        stm_stop_transaction(False) #5
        # ['\x00', '\x01', 'm', '\x03', '\xc5', '\x05', '\x06', '\x07', '\x08', '\t']
        # log: []
        #
        self.switch(0)
        stm_read(p1)
        assert p1[8] == '\x01'
        stm_write(p1)
        p1[8] = '*'
        #
        self.switch(1)
        stm_start_transaction()
        stm_read(p3)
        assert p3[8] == '\x03'
        stm_write(p3)
        p3[8] = '\xd2'
        #
        self.switch(0)
        stm_stop_transaction(False) #6
        # ['\x00', '*', 'T', '\x03', '\x0c', 'Z', '\x06', '\x07', '\x08', '\t']
        # log: [1, 2, 4, 5]
        #
        self.switch(1)
        stm_read(p7)
        assert p7[8] == '\x07'
        stm_write(p7)
        p7[8] = '.'
        #
        self.switch(0)
        stm_start_transaction()
        stm_read(p7)
        assert p7[8] == '\x07'
        #
        self.switch(1)
        stm_read(p2)
        assert p2[8] == 'm'
        stm_write(p2)
        p2[8] = '\xe9'
        #
        self.switch(1)
        stm_read(p0)
        assert p0[8] == '\x00'
        #
        self.switch(0)
        stm_read(p1)
        assert p1[8] == '*'
        #
        self.switch(0)
        stm_read(p8)
        assert p8[8] == '\x08'
        stm_write(p8)
        p8[8] = 'X'
        #
        self.switch(2)
        stm_stop_transaction(True) #7
        # conflict: 0xdf0a8018
        #
        self.switch(1)
        stm_read(p9)
        assert p9[8] == '\t'
        #
        self.switch(0)
        stm_read(p8)
        assert p8[8] == 'X'
        #
        self.switch(1)
        stm_read(p4)
        assert p4[8] == '\xc5'
        stm_write(p4)
        p4[8] = '\xb2'
        #
        self.switch(0)
        stm_read(p9)
        assert p9[8] == '\t'
        #
        self.switch(2)
        stm_start_transaction()
        stm_read(p5)
        assert p5[8] == 'Z'
        stm_write(p5)
        p5[8] = '\xfa'
        #
        self.switch(2)
        stm_read(p3)
        assert p3[8] == '\x03'
        #
        self.switch(1)
        stm_read(p9)
        assert p9[8] == '\t'
        #
        self.switch(1)
        stm_read(p8)
        assert p8[8] == '\x08'
        stm_write(p8)
        p8[8] = 'g'
        #
        self.switch(1)
        stm_read(p8)
        assert p8[8] == 'g'
        #
        self.switch(2)
        stm_read(p5)
        assert p5[8] == '\xfa'
        stm_write(p5)
        p5[8] = '\x86'
        #
        self.switch(2)
        stm_read(p6)
        assert p6[8] == '\x06'
        #
        self.switch(1)
        stm_read(p4)
        assert p4[8] == '\xb2'
        stm_write(p4)
        p4[8] = '\xce'
        #
        self.switch(2)
        stm_read(p2)
        assert p2[8] == 'T'
        stm_write(p2)
        p2[8] = 'Q'
        #
        self.switch(1)
        stm_stop_transaction(True) #8
        # conflict: 0xdf0a8028
        #
        self.switch(2)
        stm_stop_transaction(False) #9
        # ['\x00', '*', 'Q', '\x03', '\x0c', '\x86', '\x06', '\x07', '\x08', '\t']
        # log: [2, 5]
        #
        self.switch(0)
        stm_read(p0)
        assert p0[8] == '\x00'
        #
        self.switch(1)
        stm_start_transaction()
        stm_read(p3)
        assert p3[8] == '\x03'
        #
        self.switch(1)
        stm_read(p5)
        assert p5[8] == '\x86'
        #
        self.switch(2)
        stm_start_transaction()
        stm_read(p4)
        assert p4[8] == '\x0c'
        stm_write(p4)
        p4[8] = '{'
        #
        self.switch(1)
        stm_read(p2)
        assert p2[8] == 'Q'
        #
        self.switch(2)
        stm_read(p3)
        assert p3[8] == '\x03'
        stm_write(p3)
        p3[8] = 'V'
        #
        self.switch(1)
        stm_stop_transaction(False) #10
        # ['\x00', '*', 'Q', '\x03', '\x0c', '\x86', '\x06', '\x07', '\x08', '\t']
        # log: []
        #
        self.switch(1)
        stm_start_transaction()
        stm_read(p7)
        assert p7[8] == '\x07'
        #
        self.switch(2)
        stm_read(p0)
        assert p0[8] == '\x00'
        stm_write(p0)
        p0[8] = 'P'
        #
        self.switch(0)
        stm_stop_transaction(False) #11

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
