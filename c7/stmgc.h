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
typedef TLPREFIX struct stm_segment_info_s stm_segment_info_t;
typedef TLPREFIX struct stm_read_marker_s stm_read_marker_t;
typedef TLPREFIX struct stm_creation_marker_s stm_creation_marker_t;
typedef TLPREFIX char stm_char;
typedef void* stm_jmpbuf_t[5];  /* for use with __builtin_setjmp() */

struct stm_read_marker_s {
    /* In every segment, every object has a corresponding read marker.
       We assume that objects are at least 16 bytes long, and use
       their address divided by 16.  The read marker is equal to
       'STM_SEGMENT->transaction_read_version' if and only if the
       object was read in the current transaction. */
    uint8_t rm;
};

struct stm_creation_marker_s {
    /* In addition to read markers, every "line" of 256 bytes has one
       extra byte, the creation marker, located at the address divided
       by 256.  The creation marker is either non-zero if all objects in
       this line come have been allocated by the current transaction,
       or 0x00 if none of them have been.  Lines cannot contain a
       mixture of both.  Non-zero values are 0xff if in the nursery,
       and 0x01 if outside the nursery. */
    uint8_t cm;
};

struct stm_segment_info_s {
    uint8_t transaction_read_version;
    int segment_num;
    char *segment_base;
    stm_char *nursery_current;
    uintptr_t nursery_section_end;  /* forced to 1 by
                                       sync_all_threads_for_collection() */
    struct stm_thread_local_s *running_thread;
    stm_jmpbuf_t *jmpbuf_ptr;
};
#define STM_SEGMENT           ((stm_segment_info_t *)4352)

typedef struct stm_thread_local_s {
    /* every thread should handle the shadow stack itself */
    object_t **shadowstack, **shadowstack_base;
    /* the next fields are handled automatically by the library */
    int associated_segment_num;
    struct stm_thread_local_s *prev, *next;
} stm_thread_local_t;

/* this should use llvm's coldcc calling convention,
   but it's not exposed to C code so far */
void _stm_write_slowpath(object_t *);
stm_char *_stm_allocate_slowpath(ssize_t);
void _stm_become_inevitable(char*);
void _stm_start_transaction(stm_thread_local_t *, stm_jmpbuf_t *);

#ifdef STM_TESTS
bool _stm_was_read(object_t *obj);
bool _stm_was_written(object_t *obj);
uint8_t _stm_creation_marker(object_t *obj);
bool _stm_in_nursery(object_t *obj);
bool _stm_in_transaction(stm_thread_local_t *tl);
char *_stm_real_address(object_t *o);
object_t *_stm_segment_address(char *ptr);
void _stm_test_switch(stm_thread_local_t *tl);
object_t *_stm_allocate_old(ssize_t size_rounded_up);
void _stm_large_dump(void);
void _stm_start_safe_point(void);
void _stm_stop_safe_point(void);
void _stm_set_nursery_free_count(uint64_t free_count);
#endif

#define _STM_GCFLAG_WRITE_BARRIER_CALLED  0x80
#define STM_FLAGS_PREBUILT                0


/* ==================== HELPERS ==================== */
#ifdef NDEBUG
#define OPT_ASSERT(cond) do { if (!(cond)) __builtin_unreachable(); } while (0)
#else
#define OPT_ASSERT(cond) assert(cond)
#endif
#define LIKELY(x)   __builtin_expect(x, true)
#define UNLIKELY(x) __builtin_expect(x, false)
#define IMPLY(a, b) (!(a) || (b))


/* ==================== PUBLIC API ==================== */

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

/* The read barrier must be called whenever the object 'obj' is read.
   It is not required to call it before reading: it can be delayed for a
   bit, but we must still be in the same "scope": no allocation, no
   transaction commit, nothing that can potentially collect or do a safe
   point (like stm_write() on a different object).  Also, if we might
   have finished the transaction and started the next one, then
   stm_read() needs to be called again.
*/
static inline void stm_read(object_t *obj)
{
    ((stm_read_marker_t *)(((uintptr_t)obj) >> 4))->rm =
        STM_SEGMENT->transaction_read_version;
}

/* The write barrier must be called *before* doing any change to the
   object 'obj'.  If we might have finished the transaction and started
   the next one, then stm_write() needs to be called again.
   If stm_write() is called, it is not necessary to also call stm_read()
   on the same object.
*/
static inline void stm_write(object_t *obj)
{
    /* this is:
           'if (cm < 0x80 && (stm_flags & WRITE_BARRIER_CALLED) == 0)'
         where 'cm' can be 0 (not created in current transaction)
                     or 0xff (created in current transaction)
                     or 0x01 (same, but outside the nursery) */
    if (UNLIKELY(!((((stm_creation_marker_t *)(((uintptr_t)obj) >> 8))->cm |
                    obj->stm_flags) & _STM_GCFLAG_WRITE_BARRIER_CALLED)))
        _stm_write_slowpath(obj);
}

/* Must be provided by the user of this library.
   The "size rounded up" must be a multiple of 8 and at least 16.
   "Tracing" an object means enumerating all GC references in it,
   by invoking the callback passed as argument.
*/
extern ssize_t stmcb_size_rounded_up(struct object_s *);
extern void stmcb_trace(struct object_s *, void (object_t **));


/* Allocate an object of the given size, which must be a multiple
   of 8 and at least 16.  In the fast-path, this is inlined to just
   a few assembler instructions.
*/
static inline object_t *stm_allocate(ssize_t size_rounded_up)
{
    OPT_ASSERT(size_rounded_up >= 16);
    OPT_ASSERT((size_rounded_up & 7) == 0);

    stm_char *p = STM_SEGMENT->nursery_current;
    stm_char *end = p + size_rounded_up;
    STM_SEGMENT->nursery_current = end;
    if (UNLIKELY((uintptr_t)end > STM_SEGMENT->nursery_section_end))
        p = _stm_allocate_slowpath(size_rounded_up);
    return (object_t *)p;
}


/* stm_setup() needs to be called once at the beginning of the program.
   stm_teardown() can be called at the end, but that's not necessary
   and rather meant for tests.
 */
void stm_setup(void);
void stm_teardown(void);

/* Every thread needs to have a corresponding stm_thread_local_t
   structure.  It may be a "__thread" global variable or something else.
   Use the following functions at the start and at the end of a thread.
   The user of this library needs to maintain the two shadowstack fields;
   at any call to stm_allocate(), these fields should point to a range
   of memory that can be walked in order to find the stack roots.
*/
void stm_register_thread_local(stm_thread_local_t *tl);
void stm_unregister_thread_local(stm_thread_local_t *tl);

/* Starting and ending transactions.  You should only call stm_read(),
   stm_write() and stm_allocate() from within a transaction.  Use
   the macro STM_START_TRANSACTION() to start a transaction that
   can be restarted using the 'jmpbuf' (a local variable of type
   stm_jmpbuf_t). */
#define STM_START_TRANSACTION(tl, jmpbuf)  ({           \
    int _restart = __builtin_setjmp(&jmpbuf);           \
    _stm_start_transaction(tl, &jmpbuf);                \
   _restart;                                            \
})

/* Start an inevitable transaction, if it's going to return from the
   current function immediately. */
static inline void stm_start_inevitable_transaction(stm_thread_local_t *tl) {
    _stm_start_transaction(tl, NULL);
}

/* Commit a transaction. */
void stm_commit_transaction(void);

/* Abort the currently running transaction. */
void stm_abort_transaction(void) __attribute__((noreturn));

/* Turn the current transaction inevitable.  The 'jmpbuf' passed to
   STM_START_TRANSACTION() is not going to be used any more after
   this call (but the stm_become_inevitable() itself may still abort). */
static inline void stm_become_inevitable(char* msg) {
    if (STM_SEGMENT->jmpbuf_ptr != NULL)
        _stm_become_inevitable(msg);
}


/* ==================== END ==================== */

#endif
