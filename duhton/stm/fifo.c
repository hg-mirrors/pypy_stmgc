#include "fifo.h"


void fifo_init(fifo_t *self)
{
    self->ff_first = NULL;
    self->ff_last = NULL;
}

void fifo_append(fifo_t *self, fifonode_t *newnode)
{
    newnode->fn_next = NULL;
    if (self->ff_last == NULL)
        self->ff_first = newnode;
    else
        self->ff_last->fn_next = newnode;
    self->ff_last = newnode;
}

int fifo_is_empty(fifo_t *self)
{
    assert((self->ff_first == NULL) == (self->ff_last == NULL));
    return (self->ff_first == NULL);
}

int fifo_is_of_length_1(fifo_t *self)
{
    return (self->ff_first != NULL && self->ff_first == self->ff_last);
}

fifonode_t *fifo_pop_left(fifo_t *self)
{
    fifonode_t *item = self->ff_first;
    self->ff_first = item->fn_next;
    if (self->ff_first == NULL)
        self->ff_last = NULL;
    return item;
}

void fifo_steal(fifo_t *self, fifo_t *other)
{
    if (other->ff_last != NULL) {
        if (self->ff_last == NULL)
            self->ff_first = other->ff_first;
        else
            self->ff_last->fn_next = other->ff_first;
        self->ff_last = other->ff_last;
        other->ff_first = NULL;
        other->ff_last = NULL;
    }
}
