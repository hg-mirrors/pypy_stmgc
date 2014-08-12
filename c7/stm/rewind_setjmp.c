#include "rewind_setjmp.h"
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <alloca.h>


struct _rewind_jmp_moved_s {
    struct _rewind_jmp_moved_s *next;
    size_t stack_size;
    size_t shadowstack_size;
};
#define RJM_HEADER  sizeof(struct _rewind_jmp_moved_s)

#ifndef RJBUF_CUSTOM_MALLOC
#define rj_malloc malloc
#define rj_free free
#else
void *rj_malloc(size_t);
void rj_free(void *);
#endif


static void copy_stack(rewind_jmp_thread *rjthread, char *base, void *ssbase)
{
    /* Copy away part of the stack and shadowstack.
       The stack is copied between 'base' (lower limit, i.e. newest bytes)
       and 'rjthread->head->frame_base' (upper limit, i.e. oldest bytes).
       The shadowstack is copied between 'ssbase' (upper limit, newest)
       and 'rjthread->head->shadowstack_base' (lower limit, oldest).
    */
    assert(rjthread->head != NULL);
    char *stop = rjthread->head->frame_base;
    assert(stop >= base);
    void *ssstop = rjthread->head->shadowstack_base;
    assert(ssstop <= ssbase);
    struct _rewind_jmp_moved_s *next = (struct _rewind_jmp_moved_s *)
        rj_malloc(RJM_HEADER + (stop - base) + (ssbase - ssstop));
    assert(next != NULL);    /* XXX out of memory */
    next->next = rjthread->moved_off;
    next->stack_size = stop - base;
    next->shadowstack_size = ssbase - ssstop;
    memcpy(((char *)next) + RJM_HEADER, base, stop - base);
    memcpy(((char *)next) + RJM_HEADER + (stop - base), ssstop,
           ssbase - ssstop);

    rjthread->moved_off_base = stop;
    rjthread->moved_off_ssbase = ssstop;
    rjthread->moved_off = next;
}

__attribute__((noinline))
long rewind_jmp_setjmp(rewind_jmp_thread *rjthread, void *ss)
{
    if (rjthread->moved_off) {
        _rewind_jmp_free_stack_slices(rjthread);
    }
    /* all locals of this function that need to be saved and restored
       across the setjmp() should be stored inside this structure */
    struct { void *ss1; rewind_jmp_thread *rjthread1; } volatile saved =
        { ss, rjthread };

    int result;
    if (__builtin_setjmp(rjthread->jmpbuf) == 0) {
        rjthread = saved.rjthread1;
        rjthread->initial_head = rjthread->head;
        result = 0;
    }
    else {
        rjthread = saved.rjthread1;
        rjthread->head = rjthread->initial_head;
        result = rjthread->repeat_count + 1;
        /* check that the shadowstack was correctly restored */
        assert(rjthread->moved_off_ssbase == saved.ss1);
    }
    rjthread->repeat_count = result;
    copy_stack(rjthread, (char *)&saved, saved.ss1);
    return result;
}

__attribute__((noinline, noreturn))
static void do_longjmp(rewind_jmp_thread *rjthread, char *stack_free)
{
    assert(rjthread->moved_off_base != NULL);

    while (rjthread->moved_off) {
        struct _rewind_jmp_moved_s *p = rjthread->moved_off;
        char *target = rjthread->moved_off_base;
        target -= p->stack_size;
        if (target < stack_free) {
            /* need more stack space! */
            do_longjmp(rjthread, alloca(stack_free - target));
        }
        memcpy(target, ((char *)p) + RJM_HEADER, p->stack_size);

        char *sstarget = rjthread->moved_off_ssbase;
        char *ssend = sstarget + p->shadowstack_size;
        memcpy(sstarget, ((char *)p) + RJM_HEADER + p->stack_size,
               p->shadowstack_size);

        rjthread->moved_off_base = target;
        rjthread->moved_off_ssbase = ssend;
        rjthread->moved_off = p->next;
        rj_free(p);
    }
    __builtin_longjmp(rjthread->jmpbuf, 1);
}

__attribute__((noreturn))
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
    copy_stack(rjthread, rjthread->moved_off_base, rjthread->moved_off_ssbase);
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
