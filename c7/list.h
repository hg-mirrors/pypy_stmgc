#ifndef _STM_LIST_H
#define _STM_LIST_H

#include "core.h"
#include <stdlib.h>

struct stm_list_s {
    uintptr_t count;
    union {
        uintptr_t last_allocated;       /* always odd */
        //struct stm_list_s *nextlist;    /* always even */
    };
    object_t *items[];
};

struct stm_list_s *stm_list_create(void);

static inline void stm_list_free(struct stm_list_s *lst)
{
    free(lst);
}


struct stm_list_s *_stm_list_grow(struct stm_list_s *, uintptr_t);

static inline struct stm_list_s *
stm_list_append(struct stm_list_s *lst, object_t *item)
{
    uintptr_t index = lst->count++;
    if (UNLIKELY(index > lst->last_allocated))
        lst = _stm_list_grow(lst, index);
    lst->items[index] = item;
    return lst;
}

#define LIST_APPEND(lst, e) {                   \
        lst = stm_list_append(lst, e);          \
    }

static inline void stm_list_clear(struct stm_list_s *lst)
{
    lst->count = 0;
}

static inline bool stm_list_is_empty(struct stm_list_s *lst)
{
    return (lst->count == 0);
}

static inline bool stm_list_count(struct stm_list_s *lst)
{
    return lst->count;
}

static inline object_t *stm_list_pop_item(struct stm_list_s *lst)
{
    return lst->items[--lst->count];
}

static inline object_t *stm_list_item(struct stm_list_s *lst, uintptr_t index)
{
    return lst->items[index];
}

#define STM_LIST_FOREACH(lst, CODE)             \
    do {                                        \
        struct stm_list_s *_lst = (lst);        \
        uintptr_t _i;                           \
        for (_i = _lst->count; _i--; ) {        \
            object_t *item = _lst->items[_i];   \
            CODE;                               \
        }                                       \
    } while (0)


#endif
