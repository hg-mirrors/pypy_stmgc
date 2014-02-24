#include <stdlib.h>

struct list_s {
    uintptr_t count;
    uintptr_t last_allocated;
    uintptr_t items[];
};

static struct list_s *list_create(void);

static inline void list_free(struct list_s *lst)
{
    free(lst);
}

#define LIST_FREE(lst)  (list_free(lst), (lst) = NULL)


static struct list_s *_list_grow(struct list_s *, uintptr_t);

static inline struct list_s *list_append(struct list_s *lst, uintptr_t item)
{
    uintptr_t index = lst->count++;
    if (UNLIKELY(index > lst->last_allocated))
        lst = _list_grow(lst, index);
    lst->items[index] = item;
    return lst;
}

#define LIST_APPEND(lst, e)   ((lst) = list_append((lst), (uintptr_t)(e)))


static inline void list_clear(struct list_s *lst)
{
    lst->count = 0;
}

static inline bool list_is_empty(struct list_s *lst)
{
    return (lst->count == 0);
}

static inline bool list_count(struct list_s *lst)
{
    return lst->count;
}

static inline uintptr_t list_pop_item(struct list_s *lst)
{
    assert(lst->count > 0);
    return lst->items[--lst->count];
}

static inline uintptr_t list_item(struct list_s *lst, uintptr_t index)
{
    return lst->items[index];
}

#define LIST_FOREACH_R(lst, TYPE, CODE)         \
    do {                                        \
        struct list_s *_lst = (lst);            \
        uintptr_t _i;                           \
        for (_i = _lst->count; _i--; ) {        \
            TYPE item = (TYPE)_lst->items[_i];  \
            CODE;                               \
        }                                       \
    } while (0)
