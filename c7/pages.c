#define _GNU_SOURCE
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
#include "pagecopy.h"


#if defined(__i386__) || defined(__x86_64__)
#  define HAVE_FULL_EXCHANGE_INSN
#endif


uintptr_t index_page_never_used;
uint8_t flag_page_private[NB_PAGES];

uint8_t list_lock = 0;
struct stm_list_s *single_page_list;


void _stm_reset_pages()
{
    assert(!list_lock);
    if (!single_page_list)
        single_page_list = stm_list_create();
    else
        stm_list_clear(single_page_list);

    index_page_never_used = FIRST_AFTER_NURSERY_PAGE;
    
    memset(flag_page_private, 0, sizeof(flag_page_private));
}

uint8_t stm_get_page_flag(int pagenum)
{
    return flag_page_private[pagenum];
}

void stm_set_page_flag(int pagenum, uint8_t flag)
{
    assert(flag_page_private[pagenum] != flag);
    flag_page_private[pagenum] = flag;
}


void stm_pages_privatize(uintptr_t pagenum)
{
    if (flag_page_private[pagenum] == PRIVATE_PAGE)
        return;

#ifdef HAVE_FULL_EXCHANGE_INSN
    /* use __sync_lock_test_and_set() as a cheaper alternative to
       __sync_bool_compare_and_swap(). */
    int previous = __sync_lock_test_and_set(&flag_page_private[pagenum],
                                            REMAPPING_PAGE);
    if (previous == PRIVATE_PAGE) {
        flag_page_private[pagenum] = PRIVATE_PAGE;
        return;
    }
    bool was_shared = (previous == SHARED_PAGE);
#else
    bool was_shared = __sync_bool_compare_and_swap(&flag_page_private[pagenum],
                                                  SHARED_PAGE, REMAPPING_PAGE);
#endif
    if (!was_shared) {
        while (1) {
            uint8_t state = ((uint8_t volatile *)flag_page_private)[pagenum];
            if (state != REMAPPING_PAGE) {
                assert(state == PRIVATE_PAGE);
                break;
            }
            spin_loop();
        }
        return;
    }

    ssize_t pgoff1 = pagenum;
    ssize_t pgoff2 = pagenum + NB_PAGES;
    ssize_t localpgoff = pgoff1 + NB_PAGES * _STM_TL->thread_num;
    ssize_t otherpgoff = pgoff1 + NB_PAGES * (1 - _STM_TL->thread_num);

    void *localpg = object_pages + localpgoff * 4096UL;
    void *otherpg = object_pages + otherpgoff * 4096UL;

    // XXX should not use pgoff2, but instead the next unused page in
    // thread 2, so that after major GCs the next dirty pages are the
    // same as the old ones
    int res = remap_file_pages(localpg, 4096, 0, pgoff2, 0);
    if (res < 0) {
        perror("remap_file_pages");
        abort();
    }
    pagecopy(localpg, otherpg);
    write_fence();
    assert(flag_page_private[pagenum] == REMAPPING_PAGE);
    flag_page_private[pagenum] = PRIVATE_PAGE;
}



uintptr_t stm_pages_reserve(int num)
{
    /* grab free, possibly uninitialized pages */
    if (!stm_list_is_empty(single_page_list)) {
        uint8_t previous;
        while ((previous = __sync_lock_test_and_set(&list_lock, 1)))
            spin_loop();
        
        if (!stm_list_is_empty(single_page_list)) {
            uintptr_t res = (uintptr_t)stm_list_pop_item(single_page_list);
            list_lock = 0;
            return res;
        }
        
        list_lock = 0;
    }

    /* Return the index'th object page, which is so far never used. */
    uintptr_t index = __sync_fetch_and_add(&index_page_never_used, num);

    int i;
    for (i = 0; i < num; i++) {
        assert(flag_page_private[index+i] == SHARED_PAGE);
    }

    if (index + num >= NB_PAGES) {
        fprintf(stderr, "Out of mmap'ed memory!\n");
        abort();
    }
    return index;
}

void stm_pages_unreserve(uintptr_t pagenum)
{
    uint8_t previous;
    while ((previous = __sync_lock_test_and_set(&list_lock, 1)))
        spin_loop();
    
    flag_page_private[pagenum] = SHARED_PAGE;
    LIST_APPEND(single_page_list, (object_t*)pagenum);

    list_lock = 0;
}



