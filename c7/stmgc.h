#ifndef _STMGC_H
#define _STMGC_H


/* ==================== INTERNAL ==================== */

/* See "API" below. */


#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <limits.h>
#include <endian.h>
#include <unistd.h>

#if LONG_MAX == 2147483647
# error "Requires a 64-bit environment"
#endif

#if BYTE_ORDER == 1234
# define LENDIAN  1    // little endian
#elif BYTE_ORDER == 4321
# define LENDIAN  0    // big endian
#else
# error "Unsupported endianness"
#endif


#define TLPREFIX __attribute__((address_space(256)))

typedef TLPREFIX struct object_s object_t;
typedef TLPREFIX struct stm_region_info_s stm_region_info_t;
typedef TLPREFIX struct stm_read_marker_s stm_read_marker_t;
typedef TLPREFIX char stm_char;
typedef void* stm_jmpbuf_t[5];  /* for use with __builtin_setjmp() */

struct stm_read_marker_s {
    uint8_t rm;
};

struct stm_region_info_s {
    uint8_t transaction_read_version;
    stm_char *nursery_current;
    uintptr_t nursery_section_end;
    char *region_base;
    struct stm_thread_local_s *running_thread;
    stm_jmpbuf_t *jmpbuf_ptr;
};
#define STM_REGION           ((stm_region_info_t *)4352)

typedef struct stm_thread_local_s {
    /* every thread should handle the shadow stack itself */
    object_t **shadowstack, **shadowstack_base;
    /* the next fields are handled automatically by the library */
    stm_region_info_t *running_in_region;
    struct stm_thread_local_s *prev, *next;
} stm_thread_local_t;

/* this should use llvm's coldcc calling convention,
   but it's not exposed to C code so far */
void _stm_write_slowpath(object_t *);
stm_char *_stm_allocate_slowpath(ssize_t);
void _stm_become_inevitable(char*);

#ifdef STM_TESTS
bool _stm_was_read(object_t *obj);
bool _stm_was_written(object_t *obj);
bool _stm_in_nursery(object_t *obj);
char *_stm_real_address(object_t *o);
object_t *_stm_region_address(char *ptr);
#endif

#define _STM_GCFLAG_WRITE_BARRIER  0x01


/* ==================== HELPERS ==================== */
#ifdef NDEBUG
#define OPT_ASSERT(cond) do { if (!(cond)) __builtin_unreachable(); } while (0)
#else
#define OPT_ASSERT(cond) assert(cond)
#endif
#define LIKELY(x)   __builtin_expect(x, true)
#define UNLIKELY(x) __builtin_expect(x, false)
#define IMPLY(a, b) (!(a) || (b))


/* ==================== API ==================== */

/* Structure of objects
   --------------------

   Objects manipulated by the user program, and managed by this library,
   must start with a "struct object_s" field.  Pointers to any user object
   must use the "TLPREFIX struct foo *" type --- don't forget TLPREFIX.
   The best is to use typedefs like above.

   The object_s part contains some fields reserved for the STM library.
   Right now this is only one byte.
*/

struct object_s {
    uint8_t stm_flags;            /* reserved for the STM library */
};

static inline void stm_read(object_t *obj)
{
    ((stm_read_marker_t *)(((uintptr_t)obj) >> 4))->rm =
        STM_REGION->transaction_read_version;
}

static inline void stm_write(object_t *obj)
{
    if (UNLIKELY(obj->stm_flags & _STM_GCFLAG_WRITE_BARRIER))
        _stm_write_slowpath(obj);
}

/* Must be provided by the user of this library.
   The "size rounded up" must be a multiple of 8 and at least 16. */
extern ssize_t stmcb_size_rounded_up(struct object_s *);
extern void stmcb_trace(struct object_s *, void (object_t **));


static inline object_t *stm_allocate(ssize_t size_rounded_up)
{
    OPT_ASSERT(size_rounded_up >= 16);
    OPT_ASSERT((size_rounded_up & 7) == 0);

    stm_char *p = STM_REGION->nursery_current;
    stm_char *end = p + size_rounded_up;
    STM_REGION->nursery_current = end;
    if (UNLIKELY((uintptr_t)end > STM_REGION->nursery_section_end))
        p = _stm_allocate_slowpath(size_rounded_up);
    return (object_t *)p;
}

object_t *stm_allocate_prebuilt(ssize_t size_rounded_up);

void stm_setup(void);
void stm_teardown(void);
void stm_register_thread_local(stm_thread_local_t *tl);
void stm_unregister_thread_local(stm_thread_local_t *tl);

void stm_start_transaction(stm_thread_local_t *tl, stm_jmpbuf_t *jmpbuf);
void stm_start_inevitable_transaction(stm_thread_local_t *tl);
void stm_commit_transaction(void);
void stm_abort_transaction(void);

#define STM_START_TRANSACTION(tl)  ({                   \
    stm_jmpbuf_t _buf;                                  \
    int _restart = __builtin_setjmp(_buf);              \
    stm_start_transaction(tl, _buf);                    \
   _restart;                                            \
})

static inline void stm_become_inevitable(char* msg) {
    if (STM_REGION->jmpbuf_ptr != NULL)
        _stm_become_inevitable(msg);
}


/* ==================== END ==================== */

#endif
