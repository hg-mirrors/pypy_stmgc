from support import *
import py

class TestBasic(BaseTest):

    def test_allocations(self):
        self.start_transaction()
        lp1 = stm_allocate(16)
        big = GC_LAST_SMALL_SIZE+64
        lp2 = stm_allocate(big)

        self.push_root(lp1)
        self.push_root(lp2)
        stm_minor_collect()
        lp2 = self.pop_root()
        lp1 = self.pop_root()
        self.commit_transaction()

        p1 = stm_get_obj_pages(lp1)[0]
        p2 = stm_get_obj_pages(lp2)[0]
        for t in reversed(range(self.NB_THREADS)):
            self.switch(t)
            self.start_transaction()
            assert ((t == 0 and (stm_get_page_status(p1) == PAGE_ACCESSIBLE
                                 and stm_get_page_status(p2) == PAGE_ACCESSIBLE))
                    or
                    (t != 0 and (stm_get_page_status(p1) == PAGE_NO_ACCESS
                                 and stm_get_page_status(p2) == PAGE_NO_ACCESS)))
