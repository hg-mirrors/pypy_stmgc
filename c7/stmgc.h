#ifndef _STMGC_H
#define _STMGC_H


/* ==================== INTERNAL ==================== */

/* See "API" below. */


#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <limits.h>
#include <unistd.h>

#if LONG_MAX == 2147483647
# error "Requires a 64-bit environment"
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
       object was read in the current transaction.  The nurseries
       also have corresponding read markers, but they are never used. */
    uint8_t rm;
};

struct stm_segment_info_s {
    uint8_t transaction_read_version;
    int segment_num;
    char *segment_base;
    stm_char *nursery_current;
    uintptr_t nursery_end;
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
object_t *_stm_allocate_slowpath(ssize_t);
object_t *_stm_allocate_external(ssize_t);
void _stm_become_inevitable(char*);
void _stm_start_transaction(stm_thread_local_t *, stm_jmpbuf_t *);
void _stm_collectable_safe_point(void);

/* for tests, but also used in duhton: */
object_t *_stm_allocate_old(ssize_t size_rounded_up);
char *_stm_real_address(object_t *o);
#ifdef STM_TESTS
bool _stm_was_read(object_t *obj);
bool _stm_was_written(object_t *obj);
uint8_t _stm_creation_marker(object_t *obj);
bool _stm_in_nursery(object_t *obj);
bool _stm_in_transaction(stm_thread_local_t *tl);
char *_stm_get_segment_base(long index);
void _stm_test_switch(stm_thread_local_t *tl);
void _stm_largemalloc_init_arena(char *data_start, size_t data_size);
int _stm_largemalloc_resize_arena(size_t new_size);
char *_stm_largemalloc_data_start(void);
char *_stm_large_malloc(size_t request_size);
void _stm_large_free(char *data);
void _stm_large_dump(void);
void _stm_start_safe_point(void);
void _stm_stop_safe_point(void);
void _stm_set_nursery_free_count(uint64_t free_count);
long _stm_count_modified_old_objects(void);
long _stm_count_objects_pointing_to_nursery(void);
object_t *_stm_enum_modified_old_objects(long index);
object_t *_stm_enum_objects_pointing_to_nursery(long index);
#endif

#define _STM_GCFLAG_WRITE_BARRIER      0x01
#define _STM_NSE_SIGNAL                   0
#define _STM_FAST_ALLOC           (66*1024)
#define STM_FLAGS_PREBUILT   _STM_GCFLAG_WRITE_BARRIER


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
    uint32_t stm_flags;            /* reserved for the STM library */
};

/* The read barrier must be called whenever the object 'obj' is read.
   It is not required to call it before reading: it can be delayed for a
   bit, but we must still be in the same "scope": no allocation, no
   transaction commit, nothing that can potentially collect or do a safe
   point (like stm_write() on a different object).  Also, if we might
   have finished the transaction and started the next one, then
   stm_read() needs to be called again.  It can be omitted if
   stm_write() is called, or immediately after getting the object from
   stm_allocate(), as long as the rules above are respected.
*/
static inline void stm_read(object_t *obj)
{
    ((stm_read_marker_t *)(((uintptr_t)obj) >> 4))->rm =
        STM_SEGMENT->transaction_read_version;
}

/* The write barrier must be called *before* doing any change to the
   object 'obj'.  If we might have finished the transaction and started
   the next one, then stm_write() needs to be called again.  It is not
   necessary to call it immediately after stm_allocate().
*/
static inline void stm_write(object_t *obj)
{
    if (UNLIKELY((obj->stm_flags & _STM_GCFLAG_WRITE_BARRIER) != 0))
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

    if (UNLIKELY(size_rounded_up >= _STM_FAST_ALLOC))
        return _stm_allocate_external(size_rounded_up);

    stm_char *p = STM_SEGMENT->nursery_current;
    stm_char *end = p + size_rounded_up;
    STM_SEGMENT->nursery_current = end;
    if (UNLIKELY((uintptr_t)end > STM_SEGMENT->nursery_end))
        return _stm_allocate_slowpath(size_rounded_up);

    return (object_t *)p;
}


/* stm_setup() needs to be called once at the beginning of the program.
   stm_teardown() can be called at the end, but that's not necessary
   and rather meant for tests.
 */
void stm_setup(void);
void stm_teardown(void);

/* Push and pop roots from/to the shadow stack. Only allowed inside
   transaction. */
#define STM_PUSH_ROOT(tl, p)   (*((tl).shadowstack++) = (object_t *)(p))
#define STM_POP_ROOT(tl, p)    ((p) = (typeof(p))*(--(tl).shadowstack))


/* Every thread needs to have a corresponding stm_thread_local_t
   structure.  It may be a "__thread" global variable or something else.
   Use the following functions at the start and at the end of a thread.
   The user of this library needs to maintain the two shadowstack fields;
   at any call to stm_allocate(), these fields should point to a range
   of memory that can be walked in order to find the stack roots.
*/
void stm_register_thread_local(stm_thread_local_t *tl);
void stm_unregister_thread_local(stm_thread_local_t *tl);

/* Starting and ending transactions.  stm_read(), stm_write() and
   stm_allocate() should only be called from within a transaction.
   Use the macro STM_START_TRANSACTION() to start a transaction that
   can be restarted using the 'jmpbuf' (a local variable of type
   stm_jmpbuf_t). */
#define STM_START_TRANSACTION(tl, jmpbuf)  ({                   \
    int _restart = __builtin_setjmp(jmpbuf) ? _stm_duck() : 0;  \
    _stm_start_transaction(tl, &jmpbuf);                        \
   _restart;                                                    \
})
static inline int _stm_duck(void) {
    asm("/* workaround for a llvm bug */");
    return 1;
}

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

/* Forces a safe-point if needed.  Normally not needed: this is
   automatic if you call stm_allocate(). */
static inline void stm_safe_point(void) {
    if (STM_SEGMENT->nursery_end == _STM_NSE_SIGNAL)
        _stm_collectable_safe_point();
}

/* Forces a collection. */
void stm_collect(long level);


/* ==================== END ==================== */

#endif
