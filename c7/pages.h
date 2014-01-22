enum {
    /* unprivatized page seen by all threads */
    SHARED_PAGE=0,

    /* page being in the process of privatization */
    REMAPPING_PAGE,

    /* page private for each thread */
    PRIVATE_PAGE,

    /* set for SHARED pages that only contain objects belonging
       to the current transaction, so the whole page is not
       visible yet for other threads */
    UNCOMMITTED_SHARED_PAGE,
};  /* flag_page_private */


void stm_pages_privatize(uintptr_t pagenum);
uintptr_t stm_pages_reserve(int num);
uint8_t stm_get_page_flag(int pagenum);
void stm_set_page_flag(int pagenum, uint8_t flag);
void _stm_reset_page_flags(void);




