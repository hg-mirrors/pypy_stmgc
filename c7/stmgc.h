#ifndef _STMGC_H
#define _STMGC_H


/* ==================== INTERNAL ==================== */

/* See "API" below. */


#include <stddef.h>
#include <stdint.h>
#include <assert.h>
#include <limits.h>

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


enum {
    /* set if the write-barrier slowpath needs to trigger. set on all
       old objects if there was no write-barrier on it in the same
       transaction and no collection inbetween. */
    GCFLAG_WRITE_BARRIER = (1 << 0),
};


#define TLPREFIX __attribute__((address_space(256)))

typedef TLPREFIX struct object_s object_t;
typedef TLPREFIX struct stm_pub_region_info_s stm_pub_region_info_t;
typedef TLPREFIX struct stm_read_marker_s stm_read_marker_t;
typedef TLPREFIX char stm_char;
typedef void* stm_jmpbufptr_t[5];  /* for use with __builtin_setjmp() */

struct stm_read_marker_s {
    uint8_t rm;
};

struct stm_pub_region_info_s {
    uint8_t transaction_read_version;
    stm_char *nursery_current;
    uint64_t nursery_block_end;
};
#define STM_PRINFO           ((stm_pub_region_info_t *)4352)

struct stm_thread_local_s {
    object_t **shadowstack, **shadowstack_base;
    stm_jmpbufptr_t jmpbuf;
    /* internal fields follow */
    int _flags;
    struct stm_thread_local_s *_prev, *_next;
} stm_thread_local_t;

/* this should use llvm's coldcc calling convention,
   but it's not exposed to C code so far */
void _stm_write_slowpath(object_t *);


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
        STM_PRINFO->transaction_read_version;
}

static inline void stm_write(object_t *obj)
{
    if (UNLIKELY(obj->stm_flags & GCFLAG_WRITE_BARRIER))
        _stm_write_slowpath(obj);
}

/* must be provided by the user of this library */
extern ssize_t stmcb_size(struct object_s *);
extern void stmcb_trace(struct object_s *, void (object_t **));


stm_char *_stm_allocate_slowpath(ssize_t);

static inline object_t *stm_allocate(ssize_t size)
{
    assert((size % 8) == 0);
    assert(size >= 16);

    stm_char *p = STM_PRINFO->nursery_current;
    stm_char *end = p + size;
    STM_PRINFO->nursery_current = end;
    if (UNLIKELY((uint64_t)end > STM_PRINFO->nursery_block_end))
        p = _stm_allocate_slowpath(size);
    return (object_t *)p;
}

object_t *stm_allocate_prebuilt(ssize_t size);

void stm_setup(void);
void stm_teardown(void);
void stm_register_thread_local(stm_thread_local_t *tl);
void stm_unregister_thread_local(stm_thread_local_t *tl);

void stm_start_transaction(stm_thread_local_t *tl);
void stm_start_inevitable_transaction(stm_thread_local_t *tl);
void stm_commit_transaction(void);
void stm_abort_transaction(void);
void stm_become_inevitable(char* msg);

stm_thread_local_t *_stm_test_switch(stm_thread_local_t *tl);


/* ==================== END ==================== */

#endif
