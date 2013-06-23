#include "../duhton.h"


typedef struct {
    struct fifonode_s *ff_first;
    struct fifonode_s *ff_last;
} fifo_t;

typedef struct fifonode_s {
    struct fifonode_s *fn_next;
    DuObject *fn_frame;
    DuObject *fn_code;
} fifonode_t;


#define FIFO_INITIALIZER  { NULL, NULL }

void fifo_init(fifo_t *self);
void fifo_append(fifo_t *self, fifonode_t *newnode);
int fifo_is_empty(fifo_t *self);
int fifo_is_of_length_1(fifo_t *self);
fifonode_t *fifo_pop_left(fifo_t *self);
void fifo_steal(fifo_t *self, fifo_t *other);
