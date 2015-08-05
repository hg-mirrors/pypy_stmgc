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
        assert stm_get_hint_modified_recently(p1)
        assert stm_get_hint_modified_recently(p2)
        for t in reversed(range(self.NB_THREADS)):
            self.switch(t)
            self.start_transaction()

            should_be = PAGE_ACCESSIBLE if t == 0 else PAGE_NO_ACCESS
            assert (stm_get_page_status(p1) == should_be
                    and stm_get_page_status(p2) == should_be)
            # reading triggers privatization of NO_ACCESS pages
            stm_get_char(lp1)
            stm_get_char(lp2)
            should_be = PAGE_ACCESSIBLE
            assert (stm_get_page_status(p1) == should_be
                    and stm_get_page_status(p2) == should_be)

        stm_major_collect()
        assert not stm_get_hint_modified_recently(p1)
        assert not stm_get_hint_modified_recently(p2)

    def test_modification_on_write(self):
        self.start_transaction()
        lp1 = stm_allocate(16)
        big = GC_LAST_SMALL_SIZE+64
        lp2 = stm_allocate(big)

        self.push_roots([lp1, lp2])
        stm_minor_collect()
        lp1, lp2 = self.pop_roots()
        self.push_roots([lp1, lp2])

        self.commit_transaction()

        p1 = stm_get_obj_pages(lp1)[0]
        p2 = stm_get_obj_pages(lp2)[0]

        self.switch(1)

        self.start_transaction()
        assert stm_get_hint_modified_recently(p1)
        assert stm_get_hint_modified_recently(p2)
        stm_major_collect()
        assert not stm_get_hint_modified_recently(p1)
        assert not stm_get_hint_modified_recently(p2)
        stm_set_char(lp1, 'a')
        stm_set_char(lp2, 'a')
        assert stm_get_hint_modified_recently(p1)
        assert stm_get_hint_modified_recently(p2)

    def test_resharing(self):
        self.start_transaction()
        lp1 = stm_allocate(16)
        big = GC_LAST_SMALL_SIZE+64
        lp2 = stm_allocate(big)
        self.push_roots([lp1, lp2])
        stm_minor_collect()
        lp1, lp2 = self.pop_roots()
        self.push_roots([lp1, lp2])
        self.commit_transaction()
        p1 = stm_get_obj_pages(lp1)[0]
        p2 = stm_get_obj_pages(lp2)[0]
        self.switch(1)

        self.start_transaction()
        # NO_ACCESS stays NO_ACCESS
        assert stm_get_page_status(p1) == PAGE_NO_ACCESS
        assert stm_get_page_status(p2) == PAGE_NO_ACCESS
        stm_major_collect()
        stm_major_collect()
        stm_major_collect()
        assert stm_get_page_status(p1) == PAGE_NO_ACCESS
        assert stm_get_page_status(p2) == PAGE_NO_ACCESS

        self.switch(0)
        # ACCESSIBLE becomes READONLY after 2 major gcs
        self.start_transaction()
        assert stm_get_page_status(p1) == PAGE_READONLY
        assert stm_get_page_status(p2) == PAGE_READONLY
