#include "stmimpl.h"

#ifdef _GC_DEBUG
/************************************************************/

#include <sys/mman.h>

#define PAGE_SIZE  4096
#define MMAP_LENGTH  67108864   /* 64MB */

struct zone_s {
    struct zone_s *next;
    char *start;
    uint8_t active[MMAP_LENGTH / WORD];
};

static pthread_mutex_t malloc_mutex = PTHREAD_MUTEX_INITIALIZER;
static char *free_zone = NULL, *free_zone_end = NULL;
static struct zone_s *zones = NULL;

void *stm_malloc(size_t sz)
{
    pthread_mutex_lock(&malloc_mutex);

    size_t nb_pages = (sz + PAGE_SIZE - 1) / PAGE_SIZE + 1;
    if (free_zone_end - free_zone < nb_pages * PAGE_SIZE) {
        free_zone = mmap(NULL, MMAP_LENGTH, PROT_NONE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (free_zone == NULL || free_zone == MAP_FAILED) {
            fprintf(stderr, "out of memory: mmap() failed\n");
            abort();
        }
        free_zone_end = free_zone + MMAP_LENGTH;
        assert((MMAP_LENGTH % PAGE_SIZE) == 0);

        struct zone_s *z = calloc(1, sizeof(struct zone_s));
        if (z == NULL) {
            fprintf(stderr, "out of memory: malloc(zone_s) failed\n");
            abort();
        }
        z->start = free_zone;
        z->next = zones;
        zones = z;
    }

    char *result = free_zone;
    free_zone += nb_pages * PAGE_SIZE;
    pthread_mutex_unlock(&malloc_mutex);

    result += (-sz) & (PAGE_SIZE-1);
    assert(((intptr_t)(result + sz) & (PAGE_SIZE-1)) == 0);
    stm_dbgmem_used_again(result, sz, 1);
    return result;
}

static void _stm_dbgmem(void *p, size_t sz, int prot)
{
    fprintf(stderr, "_stm_dbgmem(%p, 0x%lx, %d)\n", p, (long)sz, prot);
    if (sz == 0)
        return;
    intptr_t align = ((intptr_t)p) & (PAGE_SIZE-1);
    p = ((char *)p) - align;
    sz += align;
    int err = mprotect(p, sz, prot);
    assert(err == 0);
}

void stm_free(void *p, size_t sz)
{
    _stm_dbgmem(p, sz, PROT_READ | PROT_WRITE);
    memset(p, 0xDD, sz);
    stm_dbgmem_not_used(p, sz, 1);
}

static void _stm_dbg_mark(char *p, size_t sz, uint8_t marker)
{
    long startofs, numofs, i;
    struct zone_s *z = zones;
    while (!(z->start <= p && p < (z->start + MMAP_LENGTH))) {
        z = z->next;
        assert(z);
    }
    startofs = p - z->start;
    numofs = sz;
    assert((startofs & (WORD-1)) == 0);
    assert((numofs & (WORD-1)) == 0);
    assert(startofs + numofs <= MMAP_LENGTH);
    startofs /= WORD;
    numofs /= WORD;
    for (i=0; i<numofs; i++)
        z->active[startofs + i] = marker;
}

void stm_dbgmem_not_used(void *p, size_t sz, int protect)
{
    _stm_dbg_mark(p, sz, 0);
    if (protect)
        _stm_dbgmem(p, sz, PROT_NONE);
}

void stm_dbgmem_used_again(void *p, size_t sz, int protect)
{
    _stm_dbg_mark(p, sz, 42);
    if (protect)
        _stm_dbgmem(p, sz, PROT_READ | PROT_WRITE);
}

int stm_dbgmem_is_active(void *p1, int allow_outside)
{
    char *p = (char *)p1;
    long startofs;
    uint8_t result;
    struct zone_s *z = zones;
    while (z && !(z->start <= p && p < (z->start + MMAP_LENGTH))) {
        z = z->next;
    }
    if (!z) {
        assert(allow_outside);
        return -1;
    }
    startofs = p - z->start;
    startofs /= WORD;
    result = z->active[startofs];
    assert(result == 0 || result == 42);
    return (result != 0);
}

/************************************************************/
#endif
