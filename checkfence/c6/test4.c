#include "lsl_protos.h"

typedef unsigned short uint16_t;
typedef boolean_t bool;


#define NUM_THREADS  2
#define UNDOLOG      NUM_THREADS


typedef unsigned uid_t;


typedef struct {
    uid_t    read_version;
    char     flag_modified;
    int      value1, value2;
} object_t;

typedef struct {
    int n_modified_objects;
    uid_t transaction_read_version;
} thread_local_t;

thread_local_t tl[NUM_THREADS];
object_t obj[NUM_THREADS+1];
int n_global_history;
int leader_thread_num;
lsl_lock_t undo_lock;


void setup(void)
{
    /* initialize global state */
    leader_thread_num = 0;
    n_global_history = 0;
    lsl_initlock(&undo_lock);

    /* XXX manual unrolling */
    tl[0].n_modified_objects = 0;
    tl[0].transaction_read_version = 0;
    tl[1].n_modified_objects = 0;
    tl[1].transaction_read_version = 0;
}


int fetch_and_add(volatile int *loc, int increment)
{
    int oldvalue = *loc;
    lsl_assume(lsl_cas_64(loc, oldvalue, oldvalue + increment));
    return oldvalue;
}

/* int stm_allocate(int t) */
/* { */
/*     int result = fetch_and_add(&next_free_glob, 1); */
/*     lsl_observe_output("stm_allocate", result); */

/*     obj[t][result].flag_modified = true; */
/*     return result; */
/* } */

int acquire_lock_if_leader(int t)
{
    //XXX:
    //if (leader_thread_num != t)
    //    return 0;
    lsl_lock(&undo_lock);
    if (leader_thread_num == t)
        return 1;
    lsl_unlock(&undo_lock);
    return 0;
}

void memcpy_obj_without_header(int tdst, int tsrc)
{
    obj[tdst].value1        = obj[tsrc].value1;
    obj[tdst].value2        = obj[tsrc].value2;
}

#define stm_read(t)  \
    (obj[t].read_version = tl[t].transaction_read_version)

void stm_write(int t)
{
    if (obj[t].flag_modified)
        return;   /* already modified during this transaction */

    stm_read(t);

    int is_leader = acquire_lock_if_leader(t);
    obj[t].flag_modified = true;
    tl[t].n_modified_objects = 1;
    if (is_leader) {
        memcpy_obj_without_header(UNDOLOG, t);
        lsl_unlock(&undo_lock);
    }
}

int update_to_leader(int t, int check)
{
    /* becomes the leader, and update the local copy of the objects */
    uid_t my_version = tl[t].transaction_read_version;
    int result = check;

    if (n_global_history > 0) {
        int nmod = tl[1 - t].n_modified_objects;
        lsl_unlock(&undo_lock);

        /* loop over objects in 'global_history': if they have been
           read by the current transaction, the current transaction must
           abort; then copy them out of the leader's object space ---
           which may have been modified by the leader's uncommitted
           transaction; this case will be fixed afterwards. */
        if (result)
            result = (obj[t].read_version != my_version);
        memcpy_obj_without_header(t, 1 - t);

        /* finally, loop over objects modified by the leader,
           and copy them out of the undo log.  XXX We could use
           a heuristic to avoid copying unneeded objects: it's not
           useful to copy objects that were not also present in
           the 'global_history'. */
        if (!nmod) {
            lsl_lock(&undo_lock);
            nmod = tl[1 - t].n_modified_objects;
            if (nmod)
                lsl_unlock(&undo_lock);
        }
        if (nmod) {
            memcpy_obj_without_header(t, UNDOLOG);
            lsl_lock(&undo_lock);
        }

        n_global_history = 0;
    }
    leader_thread_num = t;
    return result;
}

void update_state(int t)
{
    lsl_lock(&undo_lock);
    if (leader_thread_num != t) {
        update_to_leader(t, 0);
    }
    lsl_unlock(&undo_lock);
}

void start_transaction(int t)
{
    lsl_assert(tl[t].n_modified_objects == 0);
    lsl_assert(!obj[t].flag_modified);
    tl[t].transaction_read_version++;
}

int stop_transaction(int t)
{
    int nmod = tl[t].n_modified_objects;
    if (nmod == 0) {
        /* no modified objects in this transaction */
        return 1;
    }

    lsl_lock(&undo_lock);

    int result;
    if (leader_thread_num == t) {
        result = 1;
    }
    else {
        result = update_to_leader(t, 1);
    }

    if (result) {
        obj[t].flag_modified = false;
        n_global_history = 1;
    }
    else {
        obj[t].flag_modified = false;
    }
    tl[t].n_modified_objects = 0;

    lsl_unlock(&undo_lock);
    return result;
}


/* void A(void) */
/* { */
/*     int t = lsl_get_thread_id(); */
/*     int num = stm_allocate(t); */
/* } */

void SETUP(void)
{
    setup();
}

void SETUP100(void)
{
    int t = 0;
    setup();

    /* XXX manual unrolling */
    obj[0].flag_modified = false;
    obj[0].read_version = 0;
    obj[0].value1 = 100;
    obj[0].value2 = 200;

    obj[1].flag_modified = false;
    obj[1].read_version = 0;
    obj[1].value1 = 100;
    obj[1].value2 = 200;
}

void R0(void)
{
    int t = lsl_get_thread_id();
    int result1, result2;
    while (1) {
        start_transaction(t);
        stm_read(t);
        result1 = obj[t].value1;
        result2 = obj[t].value2;
        if (stop_transaction(t))
            break;
    }

    lsl_observe_output("R0:value1", result1);
    lsl_observe_output("R0:value2", result2);
}

void W0INC1(void)
{
    int t = lsl_get_thread_id();
    int nvalue1, nvalue2;
    //update_state(t);

    start_transaction(t);
    stm_write(t);
    nvalue1 = ++obj[t].value1;
    nvalue2 = ++obj[t].value2;
    lsl_assert(nvalue1 == obj[t].value1);
    lsl_assert(nvalue2 == obj[t].value2);
    if (!stop_transaction(t)) {
        start_transaction(t);
        stm_write(t);
        nvalue1 = ++obj[t].value1;
        nvalue2 = ++obj[t].value2;
        lsl_assert(nvalue1 == obj[t].value1);
        lsl_assert(nvalue2 == obj[t].value2);
        if (!stop_transaction(t)) {
            lsl_observe_output("XXX W0INC1 failed twice", 0);
        }
    }

    lsl_observe_output("W0INC1:nvalue1", nvalue1);
    lsl_observe_output("W0INC1:nvalue2", nvalue2);
}

void UPD(void)
{
    int t = lsl_get_thread_id();
    update_state(t);
}
