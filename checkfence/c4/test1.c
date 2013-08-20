#include "lsl_protos.h"


#define PREBUILT_FLAGS     0
#define LOCKED             5

typedef int revision_t;


typedef struct {
    int        h_flags;
    revision_t h_revision;
    revision_t h_original;
    int        value;
} object_t;

object_t o1, o2;
int global_timestamp;

struct tx_descriptor {
    int starttime;
    int lock;
    object_t *copy_of_o1;
};

void init_descriptor(struct tx_descriptor *d)
{
    d->starttime = global_timestamp; lsl_fence("load-load");
    d->copy_of_o1 = NULL;
    //d->lock = lsl_get_thread_id() + 1000000;
}


object_t *stm_write_barrier(struct tx_descriptor *d, object_t *P)
{
    lsl_observe_label("write_barrier");

    if (d->copy_of_o1 == NULL) {
        lsl_assume(P->h_revision <= d->starttime);  /* otherwise, abort */

        object_t *W = lsl_malloc(sizeof(object_t));
        W->value = P->value;
        d->copy_of_o1 = W;
    }
    return d->copy_of_o1;
}


void i()
{
    o1.h_flags    = PREBUILT_FLAGS;
    o1.h_revision = 0;
    o1.h_original = 0;
    o1.value      = 50;
    global_timestamp = 2;
}

void commit(struct tx_descriptor *d)
{
    lsl_observe_label("commit");

    if (d->copy_of_o1 != NULL) {
        int old = o1.h_revision;
        lsl_assume(old <= d->starttime);   /* otherwise, abort */
        lsl_assume(lsl_cas_32(&o1.h_revision, old, LOCKED));  /* retry */
    }

    int endtime = global_timestamp + 1;
    lsl_fence("load-load");
    lsl_assume(lsl_cas_32(&global_timestamp, endtime - 1, endtime));
    /* otherwise, retry */

    if (d->copy_of_o1 != NULL) {
        int o1_value = d->copy_of_o1->value;
        o1.value = o1_value;
        lsl_fence("store-store");
        o1.h_revision = endtime;
        lsl_observe_output("o1_value", o1_value);
        d->copy_of_o1 = NULL;
    }
}

void W1()
{
    struct tx_descriptor d;

    init_descriptor(&d);

    object_t *p1 = stm_write_barrier(&d, &o1);
    ++p1->value;

    commit(&d);
}
