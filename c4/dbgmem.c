#include "stmimpl.h"

#ifdef _GC_DEBUG
/************************************************************/

#include <sys/mman.h>

#define PAGE_SIZE  4096
#define MMAP_TOTAL  671088640   /* 640MB */

static pthread_mutex_t malloc_mutex = PTHREAD_MUTEX_INITIALIZER;
static char *zone_current = NULL, *zone_end = NULL;


static void _stm_dbgmem(void *p, size_t sz, int prot)
{
    if (sz == 0)
        return;

    assert((ssize_t)sz > 0);
    intptr_t align = ((intptr_t)p) & (PAGE_SIZE-1);
    p = ((char *)p) - align;
    sz += align;
    int err = mprotect(p, sz, prot);
    assert(err == 0);
}

void *stm_malloc(size_t sz)
{
    pthread_mutex_lock(&malloc_mutex);

    if (zone_current == NULL) {
        zone_current = mmap(NULL, MMAP_TOTAL, PROT_NONE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (zone_current == NULL || zone_current == MAP_FAILED) {
            fprintf(stderr, "not enough memory: mmap() failed\n");
            abort();
        }
        zone_end = zone_current + MMAP_TOTAL;
        assert((MMAP_TOTAL % PAGE_SIZE) == 0);
        _stm_dbgmem(zone_current, MMAP_TOTAL, PROT_NONE);
    }

    size_t nb_pages = (sz + PAGE_SIZE - 1) / PAGE_SIZE + 1;
    char *result = zone_current;
    zone_current += nb_pages * PAGE_SIZE;
    if (zone_current > zone_end) {
        fprintf(stderr, "dbgmem.c: %ld MB of memory have been exausted\n",
                (long)(MMAP_TOTAL / (1024*1024)));
        abort();
    }
    pthread_mutex_unlock(&malloc_mutex);

    result += (-sz) & (PAGE_SIZE-1);
    assert(((intptr_t)(result + sz) & (PAGE_SIZE-1)) == 0);
    _stm_dbgmem(result, sz, PROT_READ | PROT_WRITE);
    return result;
}

void stm_free(void *p, size_t sz)
{
    memset(p, 0xDD, sz);
    _stm_dbgmem(p, sz, PROT_NONE);
}

/************************************************************/
#endif
