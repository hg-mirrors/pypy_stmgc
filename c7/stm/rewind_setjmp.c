#include "rewind_setjmp.h"
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <alloca.h>


struct _rewind_jmp_moved_s {
    struct _rewind_jmp_moved_s *next;
    size_t size;
};
#define RJM_HEADER  sizeof(struct _rewind_jmp_moved_s)

#ifndef RJBUF_CUSTOM_MALLOC
#define rj_malloc malloc
#define rj_free free
#else
void *rj_malloc(size_t);
void rj_free(void *);
#endif


static void copy_stack(rewind_jmp_thread *rjthread, char *base)
{
    char *stop = rjthread->head->frame_base;
    assert(stop > base);
    struct _rewind_jmp_moved_s *next = (struct _rewind_jmp_moved_s *)
        rj_malloc(RJM_HEADER + (stop - base));
    assert(next != NULL);    /* XXX out of memory */
    next->next = rjthread->moved_off;
    next->size = stop - base;
    memcpy(((char *)next) + RJM_HEADER, base, stop - base);

    rjthread->moved_off_base = stop;
    rjthread->moved_off = next;
}

__attribute__((noinline))
int rewind_jmp_setjmp(rewind_jmp_thread *rjthread)
{
    if (rjthread->moved_off) {
        _rewind_jmp_free_stack_slices(rjthread);
    }
    rewind_jmp_thread *volatile rjthread1 = rjthread;
    int result;
    if (__builtin_setjmp(rjthread->jmpbuf) == 0) {
        rjthread = rjthread1;
        rjthread->initial_head = rjthread->head;
        result = 0;
    }
    else {
        rjthread = rjthread1;
        rjthread->head = rjthread->initial_head;
        result = 1;
    }
    copy_stack(rjthread, (char *)&rjthread1);
    return result;
}

__attribute__((noinline))
static void do_longjmp(rewind_jmp_thread *rjthread, char *stack_free)
{
    assert(rjthread->moved_off_base != NULL);

    while (rjthread->moved_off) {
        struct _rewind_jmp_moved_s *p = rjthread->moved_off;
        char *target = rjthread->moved_off_base;
        target -= p->size;
        if (target < stack_free) {
            /* need more stack space! */
            do_longjmp(rjthread, alloca(stack_free - target));
        }
        memcpy(target, ((char *)p) + RJM_HEADER, p->size);
        rjthread->moved_off_base = target;
        rjthread->moved_off = p->next;
        rj_free(p);
    }
    __builtin_longjmp(rjthread->jmpbuf, 1);
}

void rewind_jmp_longjmp(rewind_jmp_thread *rjthread)
{
    char _rewind_jmp_marker;
    do_longjmp(rjthread, &_rewind_jmp_marker);
}

__attribute__((noinline))
void _rewind_jmp_copy_stack_slice(rewind_jmp_thread *rjthread)
{
    if (rjthread->head == NULL) {
        _rewind_jmp_free_stack_slices(rjthread);
        return;
    }
    assert(rjthread->moved_off_base < (char *)rjthread->head);
    copy_stack(rjthread, rjthread->moved_off_base);
}

void _rewind_jmp_free_stack_slices(rewind_jmp_thread *rjthread)
{
    struct _rewind_jmp_moved_s *p = rjthread->moved_off;
    struct _rewind_jmp_moved_s *pnext;
    while (p) {
        pnext = p->next;
        rj_free(p);
        p = pnext;
    }
    rjthread->moved_off = NULL;
    rjthread->moved_off_base = NULL;
}
