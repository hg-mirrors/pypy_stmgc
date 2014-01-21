#ifndef _STM_CORE_H
#define _STM_CORE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>


#define TLPREFIX __attribute__((address_space(256)))

typedef TLPREFIX struct _thread_local1_s _thread_local1_t;
typedef TLPREFIX struct object_s object_t;
typedef TLPREFIX struct read_marker_s read_marker_t;
typedef TLPREFIX char localchar_t;

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

enum {
    /* set if the write-barrier slowpath needs to trigger. set on all
       old objects if there was no write-barrier on it in the same
       transaction and no collection inbetween. */
    GCFLAG_WRITE_BARRIER = (1 << 0),
    /* set on objects which are in pages visible to others (SHARED
       or PRIVATE), but not committed yet. So only visible from
       this transaction. */
    GCFLAG_NOT_COMMITTED = (1 << 1),

    GCFLAG_MOVED = (1 << 2),
};

enum {
    /* unprivatized page seen by all threads */
    SHARED_PAGE=0,

    /* page being in the process of privatization */
    REMAPPING_PAGE,

    /* page private for each thread */
    PRIVATE_PAGE,

    /* set for SHARED pages that only contain objects belonging
       to the current transaction, so the whole page is not
       visible yet for other threads */
    UNCOMMITTED_SHARED_PAGE,
};  /* flag_page_private */


struct object_s {
    uint8_t stm_flags;            /* reserved for the STM library */
    uint8_t stm_write_lock;       /* 1 if writeable by some thread */
    /* make sure it doesn't get bigger than 4 bytes for performance
     reasons */
};

struct read_marker_s {
    uint8_t rm;
};

typedef void* jmpbufptr_t[5];  /* for use with __builtin_setjmp() */

struct _thread_local1_s {
    jmpbufptr_t *jmpbufptr;
    uint8_t transaction_read_version;
    object_t **shadow_stack;
    object_t **shadow_stack_base;
};
#define _STM_TL1            ((_thread_local1_t *)4352)


/* this should use llvm's coldcc calling convention,
   but it's not exposed to C code so far */
void _stm_write_slowpath(object_t *);

#define LIKELY(x)   __builtin_expect(x, true)
#define UNLIKELY(x) __builtin_expect(x, false)


static inline void stm_read(object_t *obj)
{
    ((read_marker_t *)(((uintptr_t)obj) >> 4))->rm =
        _STM_TL1->transaction_read_version;
}

static inline void stm_write(object_t *obj)
{
    if (UNLIKELY(obj->stm_flags & GCFLAG_WRITE_BARRIER))
        _stm_write_slowpath(obj);
}

static inline void stm_push_root(object_t *obj)
{
    *(_STM_TL1->shadow_stack++) = obj;
}

static inline object_t *stm_pop_root(void)
{
    return *(--_STM_TL1->shadow_stack);
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
char *_stm_real_address(object_t *o);
object_t *_stm_tl_address(char *ptr);

bool _stm_is_young(object_t *o);
object_t *_stm_allocate_old(size_t size);

object_t *stm_allocate_prebuilt(size_t size);

void _stm_start_safe_point(void);
void _stm_stop_safe_point(void);

void stm_abort_transaction(void);

void _stm_minor_collect();
uint8_t _stm_get_page_flag(int pagenum);
#define stm_become_inevitable(msg)   /* XXX implement me! */


#endif
