#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "list.h"


#define SETSIZE(n)    (sizeof(struct stm_list_s) + ITEMSSIZE(n))
#define ITEMSSIZE(n)  ((n) * sizeof(object_t*))
#define OVERCNT(n)    (33 + ((((n) / 2) * 3) | 1))

struct stm_list_s *stm_list_create(void)
{
    uintptr_t initial_allocation = 32;
    struct stm_list_s *lst = malloc(SETSIZE(initial_allocation));
    if (lst == NULL) {
        perror("out of memory in stm_list_create");
        abort();
    }
    lst->count = 0;
    lst->last_allocated = initial_allocation - 1;
    assert(lst->last_allocated & 1);
    return lst;
}

struct stm_list_s *_stm_list_grow(struct stm_list_s *lst, uintptr_t nalloc)
{
    assert(lst->last_allocated & 1);
    nalloc = OVERCNT(nalloc);
    lst = realloc(lst, SETSIZE(nalloc));
    if (lst == NULL) {
        perror("out of memory in _stm_list_grow");
        abort();
    }
    lst->last_allocated = nalloc - 1;
    assert(lst->last_allocated & 1);
    return lst;
}
