#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <asm/prctl.h>
#include <sys/prctl.h>
#include <pthread.h>


#include "core.h"
#include "list.h"
#include "pages.h"

uint8_t flag_page_private[NB_PAGES];
uintptr_t index_page_never_used;


uint8_t _stm_get_page_flag(int pagenum)
{
    return flag_page_private[pagenum];
}


uintptr_t stm_pages_reserve(int num)
{
    /* grab free, possibly uninitialized pages */

    // XXX look in some free list first

    /* Return the index'th object page, which is so far never used. */
    uintptr_t index = __sync_fetch_and_add(&index_page_never_used, num);

    int i;
    for (i = 0; i < num; i++) {
        assert(flag_page_private[index+i] == SHARED_PAGE);
    }
    assert(flag_page_private[index] == SHARED_PAGE);
    if (index + num >= NB_PAGES) {
        fprintf(stderr, "Out of mmap'ed memory!\n");
        abort();
    }
    return index;
}




