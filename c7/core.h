#ifndef _STM_CORE_H
#define _STM_CORE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>

#define NB_PAGES            (6*256*256)    // 6*256MB
#define NB_THREADS          2
#define MAP_PAGES_FLAGS     (MAP_SHARED | MAP_ANONYMOUS | MAP_NORESERVE)
#define LARGE_OBJECT_WORDS  36
#define NB_NURSERY_PAGES    1024 // 4MB
#define LENGTH_SHADOW_STACK   163840

#define NURSERY_SECTION     (32*4096)
/* (NB_NURSERY_PAGE * 4096) % NURSERY_SECTION == 0 */


#define TOTAL_MEMORY          (NB_PAGES * 4096UL * NB_THREADS)
#define READMARKER_END        ((NB_PAGES * 4096UL) >> 4)
#define FIRST_OBJECT_PAGE     ((READMARKER_END + 4095) / 4096UL)
#define FIRST_NURSERY_PAGE    FIRST_OBJECT_PAGE
#define READMARKER_START      ((FIRST_OBJECT_PAGE * 4096UL) >> 4)
#define FIRST_READMARKER_PAGE (READMARKER_START / 4096UL)
#define FIRST_AFTER_NURSERY_PAGE  (FIRST_OBJECT_PAGE + NB_NURSERY_PAGES)
#define HEAP_PAGES            (((NB_PAGES - FIRST_AFTER_NURSERY_PAGE) * 3) / 4)



enum {
    /* set if the write-barrier slowpath needs to trigger. set on all
       old objects if there was no write-barrier on it in the same
       transaction and no collection inbetween. */
    GCFLAG_WRITE_BARRIER = (1 << 0),
    /* set on objects which are in pages visible to others (SHARED
       or PRIVATE), but not committed yet. So only visible from
       this transaction. */
    GCFLAG_NOT_COMMITTED = (1 << 1),
    /* only used during collections to mark an obj as moved out of the
       generation it was in */
    GCFLAG_MOVED = (1 << 2),
    /* objects smaller than one page and even smaller than
       LARGE_OBJECT_WORDS * 8 bytes */
    GCFLAG_SMALL = (1 << 3),
};



#define TLPREFIX __attribute__((address_space(256)))

typedef TLPREFIX struct _thread_local1_s _thread_local1_t;
typedef TLPREFIX struct object_s object_t;
typedef TLPREFIX struct alloc_for_size_s alloc_for_size_t;
typedef TLPREFIX struct read_marker_s read_marker_t;
typedef TLPREFIX char localchar_t;
typedef void* jmpbufptr_t[5];  /* for use with __builtin_setjmp() */

/* Structure of objects
   --------------------

   Objects manipulated by the user program, and managed by this library,
   must start with a "struct object_s" field.  Pointers to any user object
   must use the "TLPREFIX struct foo *" type --- don't forget TLPREFIX.
   The best is to use typedefs like above.

   The object_s part contains some fields reserved for the STM library,
   as well as a 32-bit integer field that can be freely used by the user
   program.  However, right now this field must be read-only --- i.e. it
   must never be modified on any object that may already belong to a
   past transaction; you can only set it on just-allocated objects.  The
   best is to consider it as a field that is written to only once on
   newly allocated objects.
*/


struct object_s {
    uint8_t stm_flags;            /* reserved for the STM library */
    /* make sure it doesn't get bigger than 4 bytes for performance
     reasons */
};

struct read_marker_s {
    uint8_t rm;
};

struct alloc_for_size_s {
    localchar_t *next;
    uint16_t start, stop;
    bool flag_partial_page;
};

struct _thread_local1_s {
    jmpbufptr_t *jmpbufptr;
    uint8_t transaction_read_version;
    
    int thread_num;
    uint8_t active;                /* 1 normal, 2 inevitable, 0 no trans. */
    bool need_abort;
    char *thread_base;
    struct stm_list_s *modified_objects;

    object_t **old_shadow_stack;
    object_t **shadow_stack;
    object_t **shadow_stack_base;

    struct alloc_for_size_s alloc[LARGE_OBJECT_WORDS];
    struct stm_list_s *uncommitted_objects;

    localchar_t *nursery_current;
    struct stm_list_s *old_objects_to_trace;
};
#define _STM_TL            ((_thread_local1_t *)4352)



extern char *object_pages;                    /* start of MMAP region */
extern uint8_t write_locks[READMARKER_END - READMARKER_START];

/* this should use llvm's coldcc calling convention,
   but it's not exposed to C code so far */
void _stm_write_slowpath(object_t *);


/* ==================== HELPERS ==================== */

#define LIKELY(x)   __builtin_expect(x, true)
#define UNLIKELY(x) __builtin_expect(x, false)

#define REAL_ADDRESS(object_pages, src)   ((object_pages) + (uintptr_t)(src))


static inline struct object_s *real_address(object_t *src)
{
    return (struct object_s*)REAL_ADDRESS(_STM_TL->thread_base, src);
}

static inline char *_stm_real_address(object_t *o)
{
    if (o == NULL)
        return NULL;
    assert(FIRST_OBJECT_PAGE * 4096 <= (uintptr_t)o
           && (uintptr_t)o < NB_PAGES * 4096);
    return (char*)real_address(o);
}

static inline object_t *_stm_tl_address(char *ptr)
{
    if (ptr == NULL)
        return NULL;
    
    uintptr_t res = ptr - _STM_TL->thread_base;
    assert(FIRST_OBJECT_PAGE * 4096 <= res
           && res < NB_PAGES * 4096);
    return (object_t*)res;
}

static inline char *get_thread_base(long thread_num)
{
    return object_pages + thread_num * (NB_PAGES * 4096UL);
}


static inline void spin_loop(void)
{
    asm("pause" : : : "memory");
}


static inline void write_fence(void)
{
#if defined(__amd64__) || defined(__i386__)
    asm("" : : : "memory");
#else
#  error "Define write_fence() for your architecture"
#endif
}


/* ==================== API ==================== */

static inline void stm_read(object_t *obj)
{
    ((read_marker_t *)(((uintptr_t)obj) >> 4))->rm =
        _STM_TL->transaction_read_version;
}

static inline void stm_write(object_t *obj)
{
    if (UNLIKELY(obj->stm_flags & GCFLAG_WRITE_BARRIER))
        _stm_write_slowpath(obj);
}

static inline void stm_push_root(object_t *obj)
{
    *(_STM_TL->shadow_stack++) = obj;
}

static inline object_t *stm_pop_root(void)
{
    return *(--_STM_TL->shadow_stack);
}

/* must be provided by the user of this library */
extern size_t stmcb_size(struct object_s *);
extern void stmcb_trace(struct object_s *, void (object_t **));

void _stm_restore_local_state(int thread_num);
void _stm_teardown(void);
void _stm_teardown_thread(void);
bool _stm_is_in_transaction(void);

bool _stm_was_read(object_t *obj);
bool _stm_was_written(object_t *obj);

object_t *stm_allocate(size_t size);
void stm_setup(void);
void stm_setup_thread(void);
void stm_start_transaction(jmpbufptr_t *jmpbufptr);
void stm_stop_transaction(void);


object_t *_stm_allocate_old(size_t size);

object_t *stm_allocate_prebuilt(size_t size);

void stm_abort_transaction(void);

void _stm_minor_collect();
void stm_become_inevitable(char* msg);
void stm_start_inevitable_transaction();

struct _thread_local1_s* _stm_dbg_get_tl(int thread); /* -1 is current thread */


#endif
