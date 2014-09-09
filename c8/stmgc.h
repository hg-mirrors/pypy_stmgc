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
typedef TLPREFIX char stm_char;

struct stm_segment_info_s {
    uint8_t transaction_read_version;
    int segment_num;
    char *segment_base;
    stm_char *nursery_current;
    uintptr_t nursery_end;
    struct stm_thread_local_s *running_thread;
};
#define STM_SEGMENT           ((stm_segment_info_t *)4352)


struct stm_shadowentry_s {
    /* Like stm_read_marker_s, this is a struct to enable better
       aliasing analysis in the C code. */
    object_t *ss;
};

typedef struct stm_thread_local_s {
    /* rewind_setjmp's interface */
    rewind_jmp_thread rjthread;
    struct stm_shadowentry_s *shadowstack, *shadowstack_base;

    char *mem_clear_on_abort;
    size_t mem_bytes_to_clear_on_abort;
    long last_abort__bytes_in_nursery;
    /* the next fields are handled internally by the library */
    int associated_segment_num;
    struct stm_thread_local_s *prev, *next;
    void *creating_pthread[2];
} stm_thread_local_t;

#define _STM_GCFLAG_WRITE_BARRIER      0x01
#define _STM_FAST_ALLOC           (66*1024)

void _stm_write_slowpath(object_t *);
object_t *_stm_allocate_slowpath(ssize_t);
object_t *_stm_allocate_external(ssize_t);
void _stm_become_inevitable(const char*);

object_t *_stm_allocate_old(ssize_t size_rounded_up);
char *_stm_real_address(object_t *o);
#ifdef STM_TESTS
#include <stdbool.h>
void stm_validate(void *free_if_abort);
bool _stm_was_read(object_t *obj);
bool _stm_was_written(object_t *obj);

long stm_can_move(object_t *obj);
void _stm_test_switch(stm_thread_local_t *tl);
void _stm_test_switch_segment(int segnum);
void _push_obj_to_other_segments(object_t *obj);

char *_stm_get_segment_base(long index);
bool _stm_in_transaction(stm_thread_local_t *tl);
void _stm_set_nursery_free_count(uint64_t free_count);
long _stm_count_modified_old_objects(void);
long _stm_count_objects_pointing_to_nursery(void);
object_t *_stm_enum_modified_old_objects(long index);
object_t *_stm_enum_objects_pointing_to_nursery(long index);
object_t *_stm_next_last_cl_entry();
void _stm_start_enum_last_cl_entry();
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
    *((stm_char *)(((uintptr_t)obj) >> 4)) = STM_SEGMENT->transaction_read_version;
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

    if (UNLIKELY(size_rounded_up >= _STM_FAST_ALLOC))
        return _stm_allocate_external(size_rounded_up);

    stm_char *p = STM_SEGMENT->nursery_current;
    stm_char *end = p + size_rounded_up;
    STM_SEGMENT->nursery_current = end;
    if (UNLIKELY((uintptr_t)end > STM_SEGMENT->nursery_end))
        return _stm_allocate_slowpath(size_rounded_up);

    return (object_t *)p;
}


void stm_setup(void);
void stm_teardown(void);

#define STM_SHADOW_STACK_DEPTH   163840
#define STM_PUSH_ROOT(tl, p)   ((tl).shadowstack++->ss = (object_t *)(p))
#define STM_POP_ROOT(tl, p)    ((p) = (typeof(p))((--(tl).shadowstack)->ss))
#define STM_POP_ROOT_RET(tl)   ((--(tl).shadowstack)->ss)

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
#define stm_rewind_jmp_restore_shadowstack(tl)  do {     \
    assert(rewind_jmp_armed(&(tl)->rjthread));           \
    (tl)->shadowstack = (struct stm_shadowentry_s *)     \
        rewind_jmp_restore_shadowstack(&(tl)->rjthread); \
} while (0)
#define stm_rewind_jmp_enum_shadowstack(tl, callback)    \
    rewind_jmp_enum_shadowstack(&(tl)->rjthread, callback)


long stm_start_transaction(stm_thread_local_t *tl);
void stm_start_inevitable_transaction(stm_thread_local_t *tl);

void stm_commit_transaction(void);
void stm_abort_transaction(void) __attribute__((noreturn));

void stm_collect(long level);

long stm_identityhash(object_t *obj);
long stm_id(object_t *obj);
void stm_set_prebuilt_identityhash(object_t *obj, long hash);

object_t *stm_setup_prebuilt(object_t *);


long stm_call_on_abort(stm_thread_local_t *, void *key, void callback(void *));
long stm_call_on_commit(stm_thread_local_t *, void *key, void callback(void *));



#ifdef STM_NO_AUTOMATIC_SETJMP
int stm_is_inevitable(void);
#else
static inline int stm_is_inevitable(void) {
    return !rewind_jmp_armed(&STM_SEGMENT->running_thread->rjthread);
}
#endif
static inline void stm_become_inevitable(stm_thread_local_t *tl,
                                         const char* msg) {
    assert(STM_SEGMENT->running_thread == tl);
    if (!stm_is_inevitable())
        _stm_become_inevitable(msg);
}


/* ==================== END ==================== */

#endif
