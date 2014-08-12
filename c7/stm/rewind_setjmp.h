#ifndef _REWIND_SETJMP_H_
#define _REWIND_SETJMP_H_

/************************************************************

           :                   :       ^^^^^
           |-------------------|    older frames in the stack
           |   prev=0          |
     ,---> | rewind_jmp_buf    |
     |     |-------------------|
     |     |                   |
     |     :                   :
     |     :                   :
     |     |                   |
     |     |-------------------|
     `---------prev            |
    ,----> | rewind_jmp_buf    |
    |      +-------------------|
    |      |                   |
    |      :                   :
    |      |                   |
    |      |-------------------|
    `----------prev            |
     ,---> | rewind_jmp_buf    | <--------------- MOVED_OFF_BASE
     |     |----------------  +-------------+
     |     |                  | STACK COPY  |
     |     |                  :             :
     |     :                  |  size       |
     |     |                  |  next       | <---- MOVED_OFF
     |     |                  +---|------  +-------------+
     |     |                   |  |        | STACK COPY  |
     |     |-------------------|  |        : (SEQUEL)    :
     `---------prev            |  |        :             :
HEAD-----> | rewind_jmp_buf    |  |        |             |
           |-------------------|  |        |  size       |
                                  `------> |  next=0     |
                                           +-------------+


************************************************************/

typedef struct _rewind_jmp_buf {
    char *frame_base;
    char *shadowstack_base;
    struct _rewind_jmp_buf *prev;
} rewind_jmp_buf;

typedef struct {
    rewind_jmp_buf *head;
    rewind_jmp_buf *initial_head;
    char *moved_off_base;
    char *moved_off_ssbase;
    struct _rewind_jmp_moved_s *moved_off;
    void *jmpbuf[5];
    long repeat_count;
} rewind_jmp_thread;


#define rewind_jmp_enterframe(rjthread, rjbuf, ss)   do {  \
    (rjbuf)->frame_base = __builtin_frame_address(0);      \
    (rjbuf)->shadowstack_base = (char *)(ss);              \
    (rjbuf)->prev = (rjthread)->head;                      \
    (rjthread)->head = (rjbuf);                            \
} while (0)

#define rewind_jmp_leaveframe(rjthread, rjbuf, ss)   do {    \
    assert((rjbuf)->shadowstack_base == (char *)(ss));       \
    (rjthread)->head = (rjbuf)->prev;                        \
    if ((rjbuf)->frame_base == (rjthread)->moved_off_base) { \
        assert((rjthread)->moved_off_ssbase == (char *)(ss));\
        _rewind_jmp_copy_stack_slice(rjthread);              \
    }                                                        \
} while (0)

long rewind_jmp_setjmp(rewind_jmp_thread *rjthread, void *ss);
void rewind_jmp_longjmp(rewind_jmp_thread *rjthread) __attribute__((noreturn));

#define rewind_jmp_forget(rjthread)  do {                               \
    if ((rjthread)->moved_off) _rewind_jmp_free_stack_slices(rjthread); \
    (rjthread)->moved_off_base = 0;                                     \
    (rjthread)->moved_off_ssbase = 0;                                   \
} while (0)

void _rewind_jmp_copy_stack_slice(rewind_jmp_thread *);
void _rewind_jmp_free_stack_slices(rewind_jmp_thread *);

#define rewind_jmp_armed(rjthread)   ((rjthread)->moved_off_base != 0)

#endif
