#include "lsl_protos.h"

typedef unsigned short uint16_t;
typedef boolean_t bool;


#define NUM_OBJECTS  1
#define NUM_THREADS  2
#define NUM_HISTORY  2

#define UNDOLOG      NUM_THREADS


typedef unsigned uid_t;


typedef struct {
    uid_t    read_version;
    bool     flag_modified;
    int      value1, value2;
} object_t;

typedef struct {
    int n_modified_objects;
    int modified_objects[NUM_OBJECTS];
    uid_t transaction_read_version;
} thread_local_t;

thread_local_t tl[NUM_THREADS];
object_t obj[NUM_THREADS+1][NUM_OBJECTS];
int global_history[NUM_HISTORY];
int n_global_history;
int leader_thread_num;
int next_free_glob;
lsl_lock_t undo_lock;


void setup(void)
{
    /* initialize global state */
    next_free_glob = 0;
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

int stm_allocate(int t)
{
    int result = fetch_and_add(&next_free_glob, 1);
    lsl_observe_output("stm_allocate", result);

    obj[t][result].flag_modified = true;
    return result;
}

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

void memcpy_obj_without_header(int tdst, int objndst, int tsrc, int objnsrc)
{
    obj[tdst][objndst].value1        = obj[tsrc][objnsrc].value1;
    obj[tdst][objndst].value2        = obj[tsrc][objnsrc].value2;
}

#define stm_read(t, objnum)  \
    (obj[t][objnum].read_version = tl[t].transaction_read_version)

void stm_write(int t, int objnum)
{
    if (obj[t][objnum].flag_modified)
        return;   /* already modified during this transaction */

    stm_read(t, objnum);
    obj[t][objnum].flag_modified = true;
    int n = tl[t].n_modified_objects;
    tl[t].modified_objects[n] = objnum;

    int is_leader = acquire_lock_if_leader(t);
    tl[t].n_modified_objects = n + 1;
    if (is_leader) {
        memcpy_obj_without_header(UNDOLOG, n, t, objnum);
        lsl_unlock(&undo_lock);
    }
}

int update_to_leader(int t, int check)
{
    /* becomes the leader */
    uid_t my_version = tl[t].transaction_read_version;
    int result = check;
    int nglob = n_global_history;

    if (nglob > 0) {
        while (1) {
            int objnum = global_history[--nglob];
            if (result)
                result = (obj[t][objnum].read_version != my_version);
            obj[t][objnum].flag_modified = false;
            memcpy_obj_without_header(t, objnum, 1 - t, objnum);
            if (nglob == 0)
                break;
        }
        n_global_history = 0;
    }
    int nundo = tl[1 - t].n_modified_objects;
    if (nundo > 0) {
        while (1) {
            int objnum = tl[1 - t].modified_objects[--nundo];
            if (result)
                result = (obj[t][objnum].read_version != my_version);
            obj[t][objnum].flag_modified = false;
            memcpy_obj_without_header(t, objnum, UNDOLOG, objnum);
            if (nundo == 0)
                break;
        }
    }
    return result;
}

void update_state(int t)
{
    lsl_lock(&undo_lock);
    if (leader_thread_num != t) {
        update_to_leader(t, 0);
        leader_thread_num = t;
    }
    lsl_unlock(&undo_lock);
}

void start_transaction(int t)
{
    lsl_assert(tl[t].n_modified_objects == 0);
    lsl_assert(!obj[t][0].flag_modified);
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
        if (result)
            leader_thread_num = t;
    }

    if (result) {
        int nglob = n_global_history;
        while (1) {
            int objnum = tl[t].modified_objects[--nmod];
            global_history[nglob++] = objnum;
            obj[t][objnum].flag_modified = false;
            if (nmod == 0)
                break;
        }
        n_global_history = nglob;
    }
    else {
        while (1) {
            int objnum = tl[t].modified_objects[--nmod];
            if (obj[t][objnum].flag_modified) {
                obj[t][objnum].flag_modified = false;
                memcpy_obj_without_header(t, objnum, 1 - t, objnum);
            }
            if (nmod == 0)
                break;
        }
    }
    tl[t].n_modified_objects = 0;

    lsl_unlock(&undo_lock);
    return result;
}


void A(void)
{
    int t = lsl_get_thread_id();
    int num = stm_allocate(t);
}

void SETUP(void)
{
    setup();
}

void SETUP100(void)
{
    int t = 0;
    setup();

    /* XXX manual unrolling */
    obj[0][0].flag_modified = false;
    obj[0][0].read_version = 0;
    obj[0][0].value1 = 100;
    obj[0][0].value2 = 200;

    obj[1][0].flag_modified = false;
    obj[1][0].read_version = 0;
    obj[1][0].value1 = 100;
    obj[1][0].value2 = 200;
}

void R0(void)
{
    int t = lsl_get_thread_id();
    int result1, result2;
    while (1) {
        start_transaction(t);
        stm_read(t, 0);
        result1 = obj[t][0].value1;
        result2 = obj[t][0].value2;
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
    stm_write(t, 0);
    nvalue1 = ++obj[t][0].value1;
    nvalue2 = ++obj[t][0].value2;
    lsl_assert(nvalue1 == obj[t][0].value1);
    lsl_assert(nvalue2 == obj[t][0].value2);
    if (!stop_transaction(t)) {
        start_transaction(t);
        stm_write(t, 0);
        nvalue1 = ++obj[t][0].value1;
        nvalue2 = ++obj[t][0].value2;
        lsl_assert(nvalue1 == obj[t][0].value1);
        lsl_assert(nvalue2 == obj[t][0].value2);
        if (!stop_transaction(t)) {
            lsl_observe_output("XXX W0INC1 failed twice", 0);
        }
    }

    lsl_observe_output("W0INC1:nvalue1", nvalue1);
    lsl_observe_output("W0INC1:nvalue2", nvalue2);
}
