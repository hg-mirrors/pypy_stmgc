#include "rewind_setjmp.h"
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <alloca.h>

#ifndef _STM_CORE_H_
long _has_mutex() {return 1;}
void s_mutex_lock() {}
void s_mutex_unlock() {}
#endif


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
    /* Copy away part of the stack and shadowstack. Sets moved_off_base to
       the current frame_base.

       The stack is copied between 'base' (lower limit, i.e. newest bytes)
       and 'rjthread->head->frame_base' (upper limit, i.e. oldest bytes).
       The shadowstack is copied between 'ssbase' (upper limit, newest)
       and 'rjthread->head->shadowstack_base' (lower limit, oldest).
    */
    struct _rewind_jmp_moved_s *next;
    char *stop;
    void *ssstop;
    size_t stack_size, ssstack_size;

    assert(_has_mutex());

    assert(rjthread->head != NULL);
    stop = rjthread->head->frame_base;
    ssstop = rjthread->head->shadowstack_base;
    assert(stop >= base);
    assert(ssstop <= ssbase);
    stack_size = stop - base;
    ssstack_size = ssbase - ssstop;

    next = (struct _rewind_jmp_moved_s *)
        rj_malloc(RJM_HEADER + stack_size + ssstack_size);
    assert(next != NULL);    /* XXX out of memory */
    next->next = rjthread->moved_off;
    next->stack_size = stack_size;
    next->shadowstack_size = ssstack_size;

    memcpy(((char *)next) + RJM_HEADER, base, stack_size);
    memcpy(((char *)next) + RJM_HEADER + stack_size, ssstop,
           ssstack_size);

    rjthread->moved_off_base = stop;
    rjthread->moved_off_ssbase = ssstop;
    rjthread->moved_off = next;
}

__attribute__((noinline))
long rewind_jmp_setjmp(rewind_jmp_thread *rjthread, void *ss)
{
    /* saves the current stack frame to the list of slices and
       calls setjmp(). It returns the number of times a longjmp()
       jumped back to this setjmp() */
    if (rjthread->moved_off) {
        /* old stack slices are not needed anymore (next longjmp()
           will restore only to this setjmp()) */
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
    }
    rjthread->repeat_count = result;

    /* snapshot of top frame: needed every time because longjmp() frees
       the previous one. Need to have mutex locked otherwise a concurrent
       GC may get garbage while saving shadow stack */
    s_mutex_lock();
    copy_stack(rjthread, (char *)&saved, saved.ss1);
    s_mutex_unlock();

    return result;
}

__attribute__((noinline, noreturn))
static void do_longjmp(rewind_jmp_thread *rjthread, char *stack_free)
{
    /* go through list of copied stack-slices and copy them back to the
       current stack, expanding it if necessary. The shadowstack should
       already be restored at this point (restore_shadowstack()) */
    assert(rjthread->moved_off_base != NULL);
    s_mutex_lock();

    while (rjthread->moved_off) {
        struct _rewind_jmp_moved_s *p = rjthread->moved_off;
        char *target = rjthread->moved_off_base;
        /* CPU stack grows downwards: */
        target -= p->stack_size;
        if (target < stack_free) {
            /* need more stack space! */
            s_mutex_unlock();
            do_longjmp(rjthread, alloca(stack_free - target));
            abort();            /* unreachable */
        }
        memcpy(target, ((char *)p) + RJM_HEADER, p->stack_size);

        rjthread->moved_off_base = target;
        rjthread->moved_off = p->next;
        rj_free(p);
    }

    s_mutex_unlock();
    __builtin_longjmp(rjthread->jmpbuf, 1);
}

__attribute__((noreturn))
void rewind_jmp_longjmp(rewind_jmp_thread *rjthread)
{
    char _rewind_jmp_marker;
    do_longjmp(rjthread, &_rewind_jmp_marker);
}


char *rewind_jmp_enum_shadowstack(rewind_jmp_thread *rjthread,
                                  void *callback(void *, const void *, size_t))
{
    /* enumerate all saved shadow-stack slices */
    struct _rewind_jmp_moved_s *p = rjthread->moved_off;
    char *sstarget = rjthread->moved_off_ssbase;

    assert(_has_mutex());

    while (p) {
        if (p->shadowstack_size) {
            void *ss_slice = ((char *)p) + RJM_HEADER + p->stack_size;
            callback(sstarget, ss_slice, p->shadowstack_size);

            sstarget += p->shadowstack_size;
        }
        p = p->next;
    }
    return sstarget;
}


char *rewind_jmp_restore_shadowstack(rewind_jmp_thread *rjthread)
{
    return rewind_jmp_enum_shadowstack(rjthread, memcpy);
}

__attribute__((noinline))
void _rewind_jmp_copy_stack_slice(rewind_jmp_thread *rjthread)
{
    /* called when leaving a frame. copies the now-current frame
       to the list of stack-slices */
    s_mutex_lock();
    if (rjthread->head == NULL) {
        _rewind_jmp_free_stack_slices(rjthread);
        s_mutex_unlock();
        return;
    }
    assert(rjthread->moved_off_base < (char *)rjthread->head);
    copy_stack(rjthread, rjthread->moved_off_base, rjthread->moved_off_ssbase);
    s_mutex_unlock();
}

void _rewind_jmp_free_stack_slices(rewind_jmp_thread *rjthread)
{
    /* frees all saved stack copies */
    assert(_has_mutex());
    struct _rewind_jmp_moved_s *p = rjthread->moved_off;
    while (p) {
        struct _rewind_jmp_moved_s *pnext = p->next;
        rj_free(p);
        p = pnext;
    }
    rjthread->moved_off = NULL;
    rjthread->moved_off_base = NULL;
    rjthread->moved_off_ssbase = NULL;
}
