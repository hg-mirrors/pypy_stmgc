#ifndef _STMGC_H
#define _STMGC_H


/* ==================== INTERNAL ==================== */

/* See "API" below. */


#include <stddef.h>
#include <stdint.h>
#include <assert.h>
#include <limits.h>
#include <unistd.h>

#include "stm/rewind_setjmp.h"

#if LONG_MAX == 2147483647
# error "Requires a 64-bit environment"
#endif


#define TLPREFIX __attribute__((address_space(256)))

typedef TLPREFIX struct object_s object_t;
typedef TLPREFIX struct stm_segment_info_s stm_segment_info_t;
typedef TLPREFIX struct stm_read_marker_s stm_read_marker_t;
typedef TLPREFIX char stm_char;

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
};
#define STM_SEGMENT           ((stm_segment_info_t *)4352)


typedef struct stm_thread_local_s {
    /* rewind_setjmp's interface */
    rewind_jmp_thread rjthread;
    /* the next fields are handled internally by the library */
    int associated_segment_num;
    struct stm_thread_local_s *prev, *next;
    void *creating_pthread[2];
} stm_thread_local_t;

#define _STM_GCFLAG_WRITE_BARRIER      0x01


void _stm_write_slowpath(object_t *);
object_t *_stm_allocate_slowpath(ssize_t);

object_t *_stm_allocate_old(ssize_t size_rounded_up);
char *_stm_real_address(object_t *o);
#ifdef STM_TESTS
#include <stdbool.h>
bool _stm_was_read(object_t *obj);
bool _stm_was_written(object_t *obj);

long stm_can_move(object_t *obj);
void _stm_test_switch(stm_thread_local_t *tl);
void _push_obj_to_other_segments(object_t *obj);

char *_stm_get_segment_base(long index);
bool _stm_in_transaction(stm_thread_local_t *tl);
void _stm_set_nursery_free_count(uint64_t free_count);
long _stm_count_modified_old_objects(void);
long _stm_count_objects_pointing_to_nursery(void);
object_t *_stm_enum_modified_old_objects(long index);
object_t *_stm_enum_objects_pointing_to_nursery(long index);
#endif

/* ==================== HELPERS ==================== */
#ifdef NDEBUG
#define OPT_ASSERT(cond) do { if (!(cond)) __builtin_unreachable(); } while (0)
#else
#define OPT_ASSERT(cond) assert(cond)
#endif
#define LIKELY(x)   __builtin_expect(x, 1)
#define UNLIKELY(x) __builtin_expect(x, 0)
#define IMPLY(a, b) (!(a) || (b))


/* ==================== PUBLIC API ==================== */

/* Number of segments (i.e. how many transactions can be executed in
   parallel, in maximum).  If you try to start transactions in more
   threads than the number of segments, it will block, waiting for the
   next segment to become free.
*/
#define STM_NB_SEGMENTS    4


struct object_s {
    uint32_t stm_flags;            /* reserved for the STM library */
};

extern ssize_t stmcb_size_rounded_up(struct object_s *);
void stmcb_trace(struct object_s *obj, void visit(object_t **));

__attribute__((always_inline))
static inline void stm_read(object_t *obj)
{
    ((stm_read_marker_t *)(((uintptr_t)obj) >> 4))->rm =
        STM_SEGMENT->transaction_read_version;
}

__attribute__((always_inline))
static inline void stm_write(object_t *obj)
{
    if (UNLIKELY((obj->stm_flags & _STM_GCFLAG_WRITE_BARRIER) != 0))
        _stm_write_slowpath(obj);
}


__attribute__((always_inline))
static inline object_t *stm_allocate(ssize_t size_rounded_up)
{
    OPT_ASSERT(size_rounded_up >= 16);
    OPT_ASSERT((size_rounded_up & 7) == 0);

    stm_char *p = STM_SEGMENT->nursery_current;
    stm_char *end = p + size_rounded_up;
    STM_SEGMENT->nursery_current = end;
    if (UNLIKELY((uintptr_t)end > STM_SEGMENT->nursery_end))
        return _stm_allocate_slowpath(size_rounded_up);

    return (object_t *)p;
}


void stm_setup(void);
void stm_teardown(void);


void stm_register_thread_local(stm_thread_local_t *tl);
void stm_unregister_thread_local(stm_thread_local_t *tl);

#define stm_rewind_jmp_enterprepframe(tl, rjbuf)                        \
    rewind_jmp_enterprepframe(&(tl)->rjthread, rjbuf, (tl)->shadowstack)
#define stm_rewind_jmp_enterframe(tl, rjbuf)       \
    rewind_jmp_enterframe(&(tl)->rjthread, rjbuf, (tl)->shadowstack)
#define stm_rewind_jmp_leaveframe(tl, rjbuf)       \
    rewind_jmp_leaveframe(&(tl)->rjthread, rjbuf, (tl)->shadowstack)
#define stm_rewind_jmp_setjmp(tl)                  \
    rewind_jmp_setjmp(&(tl)->rjthread, (tl)->shadowstack)
#define stm_rewind_jmp_longjmp(tl)                 \
    rewind_jmp_longjmp(&(tl)->rjthread)
#define stm_rewind_jmp_forget(tl)                  \
    rewind_jmp_forget(&(tl)->rjthread)


long stm_start_transaction(stm_thread_local_t *tl);
void stm_commit_transaction(void);
void stm_abort_transaction(void) __attribute__((noreturn));


/* ==================== END ==================== */

#endif
