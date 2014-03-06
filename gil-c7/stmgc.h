#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <unistd.h>
#include <stdlib.h>
#include <pthread.h>

#define TLPREFIX    /* nothing */


typedef struct { /* empty */ } stm_jmpbuf_t;

typedef struct object_s {
    uint32_t gil_flags;
} object_t;

typedef struct {
    object_t **shadowstack;
    object_t **shadowstack_base;
    object_t *thread_local_obj;
}  stm_thread_local_t;

extern stm_thread_local_t *_stm_tloc;
extern char *_stm_nursery_current, *_stm_nursery_end;

#ifdef NDEBUG
#define OPT_ASSERT(cond) do { if (!(cond)) __builtin_unreachable(); } while (0)
#else
#define OPT_ASSERT(cond) assert(cond)
#endif
#define UNLIKELY(x) __builtin_expect(x, false)

#define _STM_GCFLAG_WRITE_BARRIER      0x01
#define _STM_FAST_ALLOC           (66*1024)


object_t *_stm_allocate_old(ssize_t size);

object_t *_stm_allocate_external(ssize_t);
object_t *_stm_allocate_slowpath(ssize_t);

inline static object_t *stm_allocate(ssize_t size_rounded_up) {
    OPT_ASSERT(size_rounded_up >= 16);
    OPT_ASSERT((size_rounded_up & 7) == 0);

    if (UNLIKELY(size_rounded_up >= _STM_FAST_ALLOC))
        return _stm_allocate_external(size_rounded_up);

    char *p = _stm_nursery_current;
    char *end = p + size_rounded_up;
    _stm_nursery_current = end;
    if (UNLIKELY(end > _stm_nursery_end))
        return _stm_allocate_slowpath(size_rounded_up);

    return (object_t *)p;
}

inline static void stm_register_thread_local(stm_thread_local_t *tl) {
    tl->thread_local_obj = NULL;
    tl->shadowstack_base = (object_t **)malloc(768*1024);
    assert(tl->shadowstack_base);
    tl->shadowstack = tl->shadowstack_base;
}
inline static void stm_unregister_thread_local(stm_thread_local_t *tl) {
    free(tl->shadowstack_base);
}

extern pthread_mutex_t _stm_gil;

void stm_setup(void);
void stm_teardown(void);

inline static void stm_start_inevitable_transaction(stm_thread_local_t *tl) {
    if (pthread_mutex_lock(&_stm_gil) != 0) abort();
    _stm_tloc = tl;
}
inline static void stm_commit_transaction(void) {
    _stm_tloc = NULL;
    if (pthread_mutex_unlock(&_stm_gil) != 0) abort();
}
inline static void stm_become_inevitable(const char *msg) { }
inline static void stm_read(object_t *ob) { }

void _stm_write_slowpath(object_t *);

inline static void stm_write(object_t *ob) {
    if (UNLIKELY(ob->gil_flags & _STM_GCFLAG_WRITE_BARRIER))
        _stm_write_slowpath(ob);
}

inline static char *_stm_real_address(object_t *ob) { return (char *)ob; }

#define STM_START_TRANSACTION(tl, here)   do {  \
    (void)&(here);                              \
    stm_start_inevitable_transaction(tl);       \
} while (0)

#define STM_PUSH_ROOT(tl, p)   (*((tl).shadowstack++) = (object_t *)(p))
#define STM_POP_ROOT(tl, p)    ((p) = (typeof(p))*(--(tl).shadowstack))


extern ssize_t stmcb_size_rounded_up(struct object_s *);
extern void stmcb_trace(struct object_s *, void (object_t **));

inline static object_t *stm_setup_prebuilt(object_t *preb)
{
    preb->gil_flags |= _STM_GCFLAG_WRITE_BARRIER;
    return preb;
}
