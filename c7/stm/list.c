#ifndef _STM_CORE_H_
# error "must be compiled via stmgc.c"
#endif


#define LIST_SETSIZE(n)    (sizeof(struct list_s) + LIST_ITEMSSIZE(n))
#define LIST_ITEMSSIZE(n)  ((n) * sizeof(uintptr_t))
#define LIST_OVERCNT(n)    (33 + ((((n) / 2) * 3) | 1))

static struct list_s *list_create(void)
{
    uintptr_t initial_allocation = 32;
    struct list_s *lst = malloc(LIST_SETSIZE(initial_allocation));
    if (lst == NULL) {
        perror("out of memory in list_create");
        abort();
    }
    lst->count = 0;
    lst->last_allocated = initial_allocation - 1;
    return lst;
}

static struct list_s *_list_grow(struct list_s *lst, uintptr_t nalloc)
{
    nalloc = LIST_OVERCNT(nalloc);
    lst = realloc(lst, LIST_SETSIZE(nalloc));
    if (lst == NULL) {
        perror("out of memory in _list_grow");
        abort();
    }
    lst->last_allocated = nalloc - 1;
    return lst;
}
