enum {
    /* unprivatized page seen by all threads */
    SHARED_PAGE=0,

    /* page being in the process of privatization */
    REMAPPING_PAGE,

    /* page private for each thread */
    PRIVATE_PAGE,
};  /* flag_page_private */


void stm_pages_privatize(uintptr_t pagenum);
uintptr_t stm_pages_reserve(int num);
uint8_t stm_get_page_flag(int pagenum);
void stm_set_page_flag(int pagenum, uint8_t flag);
void _stm_reset_pages(void);
void stm_pages_unreserve(uintptr_t num);



