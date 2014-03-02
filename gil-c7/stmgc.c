#include "stmgc.h"
#include <string.h>
#include <stdio.h>

pthread_mutex_t _stm_gil = PTHREAD_MUTEX_INITIALIZER;
stm_thread_local_t *_stm_tloc;


/************************************************************/

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


__attribute__((unused))
static inline void list_clear(struct list_s *lst)
{
    lst->count = 0;
}

static inline bool list_is_empty(struct list_s *lst)
{
    return (lst->count == 0);
}

__attribute__((unused))
static inline uintptr_t list_count(struct list_s *lst)
{
    return lst->count;
}

static inline uintptr_t list_pop_item(struct list_s *lst)
{
    assert(lst->count > 0);
    return lst->items[--lst->count];
}

__attribute__((unused))
static inline uintptr_t list_item(struct list_s *lst, uintptr_t index)
{
    return lst->items[index];
}

__attribute__((unused))
static inline void list_set_item(struct list_s *lst, uintptr_t index,
                                 uintptr_t newitem)
{
    lst->items[index] = newitem;
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

#define LIST_SETSIZE(n)    (sizeof(struct list_s) + LIST_ITEMSSIZE(n))
#define LIST_ITEMSSIZE(n)  ((n) * sizeof(uintptr_t))
#define LIST_OVERCNT(n)    (33 + ((((n) / 2) * 3) | 1))

static struct list_s *list_create(void)
{
    uintptr_t initial_allocation = 32;
    struct list_s *lst = malloc(LIST_SETSIZE(initial_allocation));
    if (lst == NULL)
        abort();

    lst->count = 0;
    lst->last_allocated = initial_allocation - 1;
    return lst;
}

static struct list_s *_list_grow(struct list_s *lst, uintptr_t nalloc)
{
    nalloc = LIST_OVERCNT(nalloc);
    lst = realloc(lst, LIST_SETSIZE(nalloc));
    if (lst == NULL)
        abort();

    lst->last_allocated = nalloc - 1;
    return lst;
}

/************************************************************/

#define GCFLAG_WRITE_BARRIER  _STM_GCFLAG_WRITE_BARRIER

static struct list_s *objects_pointing_to_nursery;

void stm_setup(void)
{
    objects_pointing_to_nursery = list_create();
}

void stm_teardown(void)
{
    list_free(objects_pointing_to_nursery);
}

void _stm_write_slowpath(object_t *obj)
{
    obj->gil_flags &= ~GCFLAG_WRITE_BARRIER;
    LIST_APPEND(objects_pointing_to_nursery, obj);
}

object_t *_stm_allocate_old(ssize_t size)
{
    char *p = malloc(size);
    assert(p);
    memset(p, 0, size);
    ((object_t *)p)->gil_flags = STM_FLAGS_PREBUILT;
    return (object_t *)p;
}

object_t *_stm_allocate_external(ssize_t size)
{
    char *p = malloc(size);
    assert(p);
    memset(p, 0, size);
    _stm_write_slowpath((object_t *)p);
    return (object_t *)p;
}

/************************************************************/


#define NB_NURSERY_PAGES    1024          // 4MB
#define NURSERY_SIZE        (NB_NURSERY_PAGES * 4096UL)

char *_stm_nursery_base    = NULL;
char *_stm_nursery_current = NULL;
char *_stm_nursery_end     = NULL;

static bool _is_in_nursery(object_t *obj)
{
    return ((char *)obj >= _stm_nursery_base &&
            (char *)obj < _stm_nursery_end);
}

#define GCWORD_MOVED  ((object_t *) -42)

static void minor_trace_if_young(object_t **pobj)
{
    object_t *obj = *pobj;
    object_t *nobj;

    if (obj == NULL)
        return;

    if (_is_in_nursery(obj)) {
        /* If the object was already seen here, its first word was set
           to GCWORD_MOVED.  In that case, the forwarding location, i.e.
           where the object moved to, is stored in the second word in 'obj'. */
        object_t *TLPREFIX *pforwarded_array = (object_t *TLPREFIX *)obj;

        if (pforwarded_array[0] == GCWORD_MOVED) {
            *pobj = pforwarded_array[1];    /* already moved */
            return;
        }

        /* We need to make a copy of this object.
         */
        size_t size = stmcb_size_rounded_up(obj);

        nobj = malloc(size);
        assert(nobj);

        /* Copy the object  */
        memcpy(nobj, obj, size);

        /* Done copying the object. */
        //dprintf(("\t\t\t\t\t%p -> %p\n", obj, nobj));
        pforwarded_array[0] = GCWORD_MOVED;
        pforwarded_array[1] = nobj;
        *pobj = nobj;
    }

    else {
        /* The object was not in the nursery at all */
        return;
    }

    /* Must trace the object later */
    LIST_APPEND(objects_pointing_to_nursery, nobj);
}

static void collect_roots_in_nursery(void)
{
    object_t **current = _stm_tloc->shadowstack;
    object_t **base = _stm_tloc->shadowstack_base;
    while (current-- != base) {
        minor_trace_if_young(current);
    }
    minor_trace_if_young(&_stm_tloc->thread_local_obj);
}

static inline void _collect_now(object_t *obj)
{
    assert(!_is_in_nursery(obj));

    /* We must not have GCFLAG_WRITE_BARRIER so far.  Add it now. */
    assert(!(obj->gil_flags & GCFLAG_WRITE_BARRIER));
    obj->gil_flags |= GCFLAG_WRITE_BARRIER;

    /* Trace the 'obj' to replace pointers to nursery with pointers
       outside the nursery, possibly forcing nursery objects out and
       adding them to 'objects_pointing_to_nursery' as well. */
    stmcb_trace(obj, &minor_trace_if_young);
}

static void collect_oldrefs_to_nursery(void)
{
    struct list_s *lst = objects_pointing_to_nursery;

    while (!list_is_empty(lst)) {
        object_t *obj = (object_t *)list_pop_item(lst);

        _collect_now(obj);

        /* the list could have moved while appending */
        lst = objects_pointing_to_nursery;
    }
}

static void throw_away_nursery(void)
{
    if (_stm_nursery_base == NULL) {
        _stm_nursery_base = malloc(NURSERY_SIZE);
        assert(_stm_nursery_base);
        _stm_nursery_end = _stm_nursery_base + NURSERY_SIZE;
    }

    _stm_nursery_current = _stm_nursery_base;
    memset(_stm_nursery_base, 0, NURSERY_SIZE);
}

object_t *_stm_allocate_slowpath(ssize_t size_rounded_up)
{
    /* run minor collection */
    //fprintf(stderr, "minor collect\n");
    collect_roots_in_nursery();
    collect_oldrefs_to_nursery();
    throw_away_nursery();

    char *p = _stm_nursery_current;
    char *end = p + size_rounded_up;
    assert(end <= _stm_nursery_end);
    _stm_nursery_current = end;
    return (object_t *)p;
}
