

class DISABLED:

    def test_large_obj_alloc(self):
        # test obj which doesn't fit into the size_classes
        # for now, we will still allocate it in the nursery.
        # expects: LARGE_OBJECT_WORDS  36
        size_class = 1000 # too big
        obj_size = size_class * 8
        assert obj_size > 4096 # we want more than 1 page
        assert obj_size < 4096 * 1024 # in the nursery

        self.start_transaction()
        new = stm_allocate(obj_size)
        assert is_in_nursery(new)
        assert len(stm_get_obj_pages(new)) == 2
        assert ([stm_get_page_flag(p) for p in stm_get_obj_pages(new)]
                == [lib.PRIVATE_PAGE]*2)
        self.push_root(new)
        stm_minor_collect()
        new = self.pop_root()

        assert len(stm_get_obj_pages(new)) == 2
        # assert ([stm_get_page_flag(p) for p in stm_get_obj_pages(new)]
        #         == [lib.UNCOMMITTED_SHARED_PAGE]*2)

        assert not is_in_nursery(new)

    def test_large_obj_write(self):
        # test obj which doesn't fit into the size_classes
        # expects: LARGE_OBJECT_WORDS  36
        size_class = 1000 # too big
        obj_size = size_class * 8
        assert obj_size > 4096 # we want more than 1 page
        assert obj_size < 4096 * 1024 # in the nursery

        self.start_transaction()
        new = stm_allocate(obj_size)
        assert is_in_nursery(new)
        self.push_root(new)
        self.commit_transaction()
        new = self.pop_root()

        assert ([stm_get_page_flag(p) for p in stm_get_obj_pages(new)]
                == [lib.SHARED_PAGE]*2)

        self.start_transaction()
        stm_write(new)
        assert ([stm_get_page_flag(p) for p in stm_get_obj_pages(new)]
                == [lib.PRIVATE_PAGE]*2)
        
        # write to 2nd page of object!!
        wnew = stm_get_real_address(new)
        wnew[4097] = 'x'

        self.switch(1)
        self.start_transaction()
        stm_read(new)
        rnew = stm_get_real_address(new)
        assert rnew[4097] == '\0'
        
    def test_partial_alloced_pages(self):
        self.start_transaction()
        new = stm_allocate(16)
        self.push_root(new)
        stm_minor_collect()
        new = self.pop_root()
        # assert stm_get_page_flag(stm_get_obj_pages(new)[0]) == lib.UNCOMMITTED_SHARED_PAGE
        # assert not (stm_get_flags(new) & lib.GCFLAG_NOT_COMMITTED)

        self.commit_transaction()
        assert stm_get_page_flag(stm_get_obj_pages(new)[0]) == lib.SHARED_PAGE
        assert not (stm_get_flags(new) & lib.GCFLAG_NOT_COMMITTED)

        self.start_transaction()
        newer = stm_allocate(16)
        self.push_root(newer)
        stm_minor_collect()
        newer = self.pop_root()
        # 'new' is still in shared_page and committed
        assert stm_get_page_flag(stm_get_obj_pages(new)[0]) == lib.SHARED_PAGE
        assert not (stm_get_flags(new) & lib.GCFLAG_NOT_COMMITTED)
        # 'newer' is now part of the SHARED page with 'new', but
        # marked as UNCOMMITTED, so no privatization has to take place:
        assert stm_get_obj_pages(new) == stm_get_obj_pages(newer)
        assert stm_get_flags(newer) & lib.GCFLAG_NOT_COMMITTED
        stm_write(newer) # does not privatize
        assert stm_get_page_flag(stm_get_obj_pages(newer)[0]) == lib.SHARED_PAGE
        self.commit_transaction()
        
        assert stm_get_page_flag(stm_get_obj_pages(newer)[0]) == lib.SHARED_PAGE
        assert not (stm_get_flags(newer) & lib.GCFLAG_NOT_COMMITTED)
