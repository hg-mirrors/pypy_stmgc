#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <pthread.h>
#include <semaphore.h>
#include <time.h>

#include "stmgc.h"
#include "stmimpl.h"
#include "fprintcolor.h"

extern revision_t get_private_rev_num(void);


#define NUMTHREADS 4
#define STEPS_PER_THREAD 5000
#define THREAD_STARTS 100 // how many restarts of threads
#define NUMROOTS 10 // per thread
#define PREBUILT 3 // per thread
#define MAXROOTS 1000
#define SHARED_ROOTS 5 // shared by threads
#define DO_MAJOR_COLLECTS 1



// SUPPORT
#define GCTID_STRUCT_NODE     123

struct node {
    struct stm_object_s hdr;
    long value;
    revision_t id;
    revision_t hash;
    struct node *next;
};
typedef struct node * nodeptr;

size_t stmcb_size(gcptr ob)
{
    assert(stm_get_tid(ob) == GCTID_STRUCT_NODE);
    return sizeof(struct node);
}
void stmcb_trace(gcptr ob, void visit(gcptr *))
{
    nodeptr n;
    assert(stm_get_tid(ob) == GCTID_STRUCT_NODE);
    n = (nodeptr)ob;
    visit((gcptr *)&n->next);
}


// global and per-thread-data
time_t default_seed;
gcptr shared_roots[SHARED_ROOTS];

#define CACHE_MASK 65535
#define CACHE_ENTRIES ((CACHE_MASK + 1) / sizeof(char *))
#define CACHE_AT(cache, obj) (*(gcptr *)((char *)(cache)               \
                                         + ((revision_t)(obj) & CACHE_MASK)))

struct thread_data {
    unsigned int thread_seed;
    gcptr roots[MAXROOTS];
    gcptr roots_outside_perform[MAXROOTS];
    int num_roots;
    int num_roots_outside_perform;
    int steps_left;
    int interruptible;
    int atomic;
    revision_t writeable[CACHE_ENTRIES];
};
__thread struct thread_data td;


// helper functions
int classify(gcptr p);
void check(gcptr p);

static int is_private(gcptr P)
{
  return (P->h_revision == stm_private_rev_num) ||
    (P->h_tid & GCFLAG_PRIVATE_FROM_PROTECTED);
}

static void inc_atomic()
{
    assert(td.interruptible);
    assert(stm_atomic(0) == td.atomic);
    td.atomic++;
    stm_atomic(1);
    assert(stm_atomic(0) == td.atomic);
}

static void dec_atomic()
{
    assert(td.interruptible);
    assert(stm_atomic(0) == td.atomic);
    td.atomic--;
    stm_atomic(-1);
    assert(stm_atomic(0) == td.atomic);
}

int get_rand(int max)
{
    return (int)(rand_r(&td.thread_seed) % (unsigned int)max);
}

void copy_roots(gcptr *from, gcptr *to, int num)
{
    int i;
    for (i = 0; i < num; i++)
        *(to++) = *(from++);
}

gcptr allocate_old(size_t size, int tid)
{
    gcptr p = stmgcpage_malloc(size);
    memset(p, 0, size);
    p->h_tid = GCFLAG_OLD | GCFLAG_WRITE_BARRIER | tid;
    p->h_revision = -INT_MAX;
    return p;
}

gcptr allocate_pseudoprebuilt(size_t size, int tid)
{
    gcptr x = calloc(1, size);
    x->h_tid = PREBUILT_FLAGS | tid;
    x->h_revision = PREBUILT_REVISION;
    return x;
}

gcptr allocate_pseudoprebuilt_with_hash(size_t size, int tid,
                                        revision_t hash)
{
    gcptr x = allocate_pseudoprebuilt(size, tid);
    x->h_original = hash;
    return x;
}

void push_roots(int with_cache)
{
    int i;
    for (i = 0; i < td.num_roots; i++) {
        check(td.roots[i]);
        if (td.roots[i])
            stm_push_root(td.roots[i]);
    }

    if (with_cache) {
        stm_push_root(NULL);
        for (i = 0; i < CACHE_ENTRIES; i++) {
            if (td.writeable[i])
                stm_push_root((gcptr)td.writeable[i]);
        }
    }
}

void pop_roots(int with_cache)
{
    int i;
    /* some objects may have changed positions */
    memset(td.writeable, 0, sizeof(td.writeable));

    if (with_cache) {
        gcptr obj = stm_pop_root();
        while (obj) {
            CACHE_AT(td.writeable, obj) = obj;
            obj = stm_pop_root();
        }
    }

    for (i = td.num_roots - 1; i >= 0; i--) {
        if (td.roots[i])
            td.roots[i] = stm_pop_root();
        check(td.roots[i]);
    }
}

void del_root(int idx)
{
    int i;
    for (i = idx; i < td.num_roots - 1; i++)
        td.roots[i] = td.roots[i + 1];
}

nodeptr allocate_node()
{
    nodeptr r;
    push_roots(1);
    r = (nodeptr)stm_allocate(sizeof(struct node), GCTID_STRUCT_NODE);
    pop_roots(1);
    return r;
}

int is_shared_prebuilt(gcptr p)
{
    int i;
    for (i = 0; i < SHARED_ROOTS; i++)
        if (shared_roots[i] == p)
            return 1;
    return 0;
}

#ifdef _GC_DEBUG
int is_free_old(gcptr p)
{
    fprintf(stdout, "\n=== check ===\n");
    return (!_stm_can_access_memory((char*)p))
        || (p->h_tid == DEBUG_WORD(0xDD));
}
#endif

void check_not_free(gcptr p)
{
    assert(p != NULL);
    assert((p->h_tid & 0xFFFF) == GCTID_STRUCT_NODE);
    if (is_shared_prebuilt(p))
        assert(p->h_tid & GCFLAG_PREBUILT_ORIGINAL);
}

void check(gcptr p)
{
    if (p != NULL) {
        check_not_free(p);
        classify(p); // additional asserts
        if (p->h_original && !(p->h_tid & GCFLAG_PREBUILT_ORIGINAL)) {
            // must point to valid old object
            gcptr id = (gcptr)p->h_original;
            assert(id->h_tid & GCFLAG_OLD);
            check_not_free(id);
#ifdef _GC_DEBUG
            if (!is_shared_prebuilt(id) && !(id->h_tid & GCFLAG_PREBUILT))
                assert(!is_free_old(id));
#endif
        }
    }
}

gcptr read_barrier(gcptr p)
{
    gcptr r = p;
    if (p != NULL) {
        check(p);
        r = stm_read_barrier(p);
        check(r);
    }
    return r;
}

gcptr write_barrier(gcptr p)
{
    gcptr w = p;
    if (p != NULL) {
        check(p);
        w = stm_write_barrier(p);
        CACHE_AT(td.writeable, w) = w;
        check(w);
        assert(is_private(w));
    }
    return w;
}

int in_nursery(gcptr obj)
{
    struct tx_descriptor *d = thread_descriptor;
    int result1 = (d->nursery_base <= (char*)obj &&
                   ((char*)obj) < d->nursery_end);
    if (obj->h_tid & GCFLAG_OLD) {
        assert(result1 == 0);
    }
    else {
        /* this assert() also fails if "obj" is in another nursery than
           the one of the current thread.  This is ok, because we
           should not see such pointers. */
        assert(result1 == 1);
    }
    return result1;
}

static const int C_PRIVATE_FROM_PROTECTED = 1;
static const int C_PRIVATE                = 2;
static const int C_STUB                   = 3;
static const int C_PUBLIC                 = 4;
static const int C_BACKUP                 = 5;
static const int C_PROTECTED              = 6;
int classify(gcptr p)
{
    int priv_from_prot = (p->h_tid & GCFLAG_PRIVATE_FROM_PROTECTED) != 0;
    int private_other = p->h_revision == get_private_rev_num();
    int public = (p->h_tid & GCFLAG_PUBLIC) != 0;
    int backup = (p->h_tid & GCFLAG_BACKUP_COPY) != 0;
    int stub = (p->h_tid & GCFLAG_STUB) != 0;
    assert(priv_from_prot + private_other + public + backup <= 1);
    assert(public || !stub);
    
    if (priv_from_prot)
        return C_PRIVATE_FROM_PROTECTED;
    if (private_other)
        return C_PRIVATE;
    if (public) {
        if (stub) {
            assert(!in_nursery(p));
        }
        else {
            if (in_nursery(p)) {
                assert(p->h_tid & GCFLAG_NURSERY_MOVED);
                assert(!(p->h_revision & 1));
            }
            return C_PUBLIC;
        }
    }
    if (backup)
        return C_BACKUP;
    return C_PROTECTED;
}




// THREAD TESTER
int interruptible_callback(gcptr arg1, int retry_counter);
int run_me();
void transaction_break();

void setup_thread()
{
    int i;
    memset(&td, 0, sizeof(struct thread_data));

    td.thread_seed = default_seed++;
    td.steps_left = STEPS_PER_THREAD;
    td.interruptible = 0;
    td.atomic = 0;

    td.num_roots = PREBUILT + NUMROOTS;
    for (i = 0; i < PREBUILT; i++) {
        if (i % 3 == 0) {
            td.roots[i] = allocate_pseudoprebuilt_with_hash(
                                          sizeof(struct node), 
                                          GCTID_STRUCT_NODE,
                                          i);
            ((nodeptr)td.roots[i])->hash = i;
        }
        else {
            td.roots[i] = allocate_pseudoprebuilt(sizeof(struct node), 
                                                  GCTID_STRUCT_NODE);
        }
    }
    for (i = PREBUILT; i < PREBUILT + NUMROOTS; i++) {
        td.roots[i] = (gcptr)allocate_node();
    }

    if (td.thread_seed % 3 == 0) {
        stm_thread_local_obj = (gcptr)allocate_node();
    }
    else if (td.thread_seed % 3 == 1) {
        stm_thread_local_obj = allocate_pseudoprebuilt_with_hash
            (
             sizeof(struct node), GCTID_STRUCT_NODE, PREBUILT);
        ((nodeptr)stm_thread_local_obj)->hash = PREBUILT;
    } 
    else {
        stm_thread_local_obj = allocate_pseudoprebuilt
            (sizeof(struct node), GCTID_STRUCT_NODE);
    }

}

gcptr rare_events(gcptr p, gcptr _r, gcptr _sr)
{
    int k = get_rand(100);
    if (k < 10) {
        push_roots(1);
        stm_push_root(p);
        stm_become_inevitable("fun");
        p = stm_pop_root();
        pop_roots(1);
    } 
    else if (k < 40) {
        push_roots(1);
        stmgc_minor_collect();
        pop_roots(1);
        p = NULL;
    } else if (k < 41 && DO_MAJOR_COLLECTS) {
        fprintf(stdout, "major collect\n");
        push_roots(1);
        stmgcpage_possibly_major_collect(1);
        pop_roots(1);
        p = NULL;
    }
    return p;
}

gcptr simple_events(gcptr p, gcptr _r, gcptr _sr)
{
    nodeptr w_r;
    int k = get_rand(11);
    int num = get_rand(td.num_roots);
    switch (k) {
    case 0: // remove a root
        if (num > 0)
            del_root(num);
        break;
    case 1: // add 'p' to roots
        if (p && td.num_roots < MAXROOTS)
            td.roots[td.num_roots++] = p;
        break;
    case 2: // set 'p' to point to a root
        if (_r)
            p = _r;
        break;
    case 3: // allocate fresh 'p'
        p = (gcptr)allocate_node();
        break;
    case 4:  // read and validate 'p'
        p = read_barrier(p);
        break;
    case 5: // only do a stm_write_barrier
        p = write_barrier(p);
        break;
    case 6: // follow p->next
        if (p)
            p = (gcptr)(((nodeptr)read_barrier(p))->next);
        break;
    case 7: // set 'p' as *next in one of the roots
        check(_r);
        if (CACHE_AT(td.writeable, _r) == _r)
            w_r = (nodeptr)_r;
        else
            w_r = (nodeptr)write_barrier(_r);
        check((gcptr)w_r);
        check(p);
        w_r->next = (struct node*)p;
        break;
    case 8:
        if (td.interruptible) {
            inc_atomic();
        }
        break;
    case 9:
    case 10:
        /* more likely to be less atomic */
        if (td.atomic) {
            dec_atomic();
        }
        break;
    }
    return p;
}

gcptr shared_roots_events(gcptr p, gcptr _r, gcptr _sr)
{
    nodeptr w_sr;

    int k = get_rand(3);
    switch (k) {
    case 0: // read_barrier on shared root
        read_barrier(_sr);
        break;
    case 1: // write_barrier on shared root
        write_barrier(_sr);
        break;
    case 2:
        w_sr = (nodeptr)write_barrier(_sr);
        w_sr->next = (nodeptr)shared_roots[get_rand(SHARED_ROOTS)];
        break;
    }
    return p;
}

gcptr id_hash_events(gcptr p, gcptr _r, gcptr _sr)
{
    nodeptr w_t;
    int k = get_rand(4);
    gcptr _t = NULL;

    switch (k) {
    case 0: /* test stm_id on (non-)shared roots */
        _t = _r;
    case 1:
        if (!_t)
            _t = _sr;
        w_t = (nodeptr)read_barrier(_t);
        if (w_t->id) {
            assert(w_t->id == stm_id((gcptr)w_t));
            assert(w_t->id == stm_id((gcptr)_t));
        }
        else {
            if (CACHE_AT(td.writeable, _t) == _t)
                w_t = (nodeptr)_t;
            else
                w_t = (nodeptr)write_barrier(_t);
            w_t->id = stm_id((gcptr)w_t);
            assert(w_t->id == stm_id((gcptr)_t));
        }
        break;
    case 2: /* test stm_hash on (non-)shared roots */
        _t = _r;
    case 3:
        if (!_t)
            _t = _sr;
        w_t = (nodeptr)read_barrier(_t);
        if (w_t->hash) {
            assert(w_t->hash == stm_hash((gcptr)w_t));
            assert(w_t->hash == stm_hash((gcptr)_t));
        }
        else {
            if (CACHE_AT(td.writeable, _t) == _t)
                w_t = (nodeptr)_t;
            else
                w_t = (nodeptr)write_barrier(_t);
            w_t->hash = stm_hash((gcptr)w_t);
            assert(w_t->hash == stm_hash((gcptr)_t));
        }
        if (w_t->hash >= 0 && (w_t->hash <= PREBUILT ||
                               w_t->hash < SHARED_ROOTS)) {
            // should be with predefined hash
            assert (stm_id((gcptr)w_t) != stm_hash((gcptr)w_t));
        }
        break;
    }
    return p;
}



gcptr do_step(gcptr p)
{
    gcptr _r, _sr;
    int num, k;

    num = get_rand(td.num_roots+1);
    if (num == 0)
        _r = stm_thread_local_obj;
    else
        _r = td.roots[num - 1];
    
    num = get_rand(SHARED_ROOTS);
    _sr = shared_roots[num];

    k = get_rand(9);
    check(p);
    assert(thread_descriptor->active);

    if (k < 3)
        p = simple_events(p, _r, _sr);
    else if (k < 5)
        p = shared_roots_events(p, _r, _sr);
    else if (k < 7)
        p = id_hash_events(p, _r, _sr);
    else if (k < 8)
        p = rare_events(p, _r, _sr);
    else if (get_rand(20) == 1) {
        // transaction break
        fprintf(stdout, "|");
        if (td.interruptible)
            return (gcptr)-1; // break current
        transaction_break();
        p = NULL;
    }
    return p;
}


void transaction_break()
{
    push_roots(0);
    td.interruptible = 1;
    
    copy_roots(td.roots, td.roots_outside_perform, td.num_roots);
    td.num_roots_outside_perform = td.num_roots;
    
    stm_perform_transaction(NULL, interruptible_callback);
    
    td.num_roots = td.num_roots_outside_perform;
    copy_roots(td.roots_outside_perform, td.roots, td.num_roots);
    
    td.interruptible = 0;
    pop_roots(0);

    /* done by pop_roots() memset(&td.writeable, 0, sizeof(td.writeable)); */
}


int interruptible_callback(gcptr arg1, int retry_counter)
{
    td.num_roots = td.num_roots_outside_perform;
    // done & overwritten by the following pop_roots():
    // copy_roots(td.roots_outside_perform, td.roots, td.num_roots);
    td.atomic = 0; // may be set differently on abort
    // refresh td.roots:
    gcptr end_marker = stm_pop_root();
    assert(end_marker == END_MARKER_ON || end_marker == END_MARKER_OFF);
    arg1 = stm_pop_root();
    assert(arg1 == NULL);
    pop_roots(0);
    push_roots(0);
    stm_push_root(arg1);
    stm_push_root(end_marker);

    int p = run_me();

    if (p == -1) // maybe restart transaction
        return get_rand(3) != 1;

    return 0;
}

int run_me()
{
    gcptr p = NULL;

    // clear cache of writeables:
    memset(&td.writeable, 0, sizeof(td.writeable));

    while (td.steps_left-->0 || td.atomic) {
        if (td.steps_left % 8 == 0)
            fprintf(stdout, "#");

        p = do_step(p);

        if (p == (gcptr)-1) {
            if (td.atomic) {
                // can't break, well, we could return to perform_transaction
                // while being atomic. (TODO)
                // may be true when major gc requested:
                // assert(stm_should_break_transaction() == 0);
                assert(stm_atomic(0) == td.atomic);
                p = NULL;
            }
            else {
                assert(stm_atomic(0) == 0);
                return -1;
            }
        }
    }
    return 0;
}


static sem_t done;
static int thr_mynum = 0;

void *demo(void *arg)
{  
    int status;
    
    stm_initialize();
    setup_thread();
    
    thr_mynum++;   /* protected by being inevitable here */
    dprintf(("THREAD STARTING\n"));

    run_me();
    
    stm_finalize();
    
    status = sem_post(&done);
    assert(status == 0);
    return NULL;
}


void newthread(void*(*func)(void*), void *arg)
{
    pthread_t th;
    int status = pthread_create(&th, NULL, func, arg);
    if (status != 0)
        stm_fatalerror("newthread: pthread_create failure\n");
    pthread_detach(th);
    printf("started new thread\n");
}



int main(void)
{
    int i, status;
    
    // seed changes daily
    // a bit pointless for now..
    default_seed = time(NULL);
    default_seed -= (default_seed % (3600 * 24));
    
    for (i = 0; i < SHARED_ROOTS; i++) {
        if (i % 3 == 0) {
            shared_roots[i] = allocate_pseudoprebuilt_with_hash(
                                          sizeof(struct node), 
                                          GCTID_STRUCT_NODE,
                                          i);
            ((nodeptr)shared_roots[i])->hash = i;
        }
        else {
            shared_roots[i] = allocate_pseudoprebuilt(sizeof(struct node), 
                                                      GCTID_STRUCT_NODE);
        }
    }    
    
    status = sem_init(&done, 0, 0);
    assert(status == 0);
    
    int thread_starts = NUMTHREADS * THREAD_STARTS;
    for (i = 0; i < NUMTHREADS; i++) {
        newthread(demo, NULL);
        thread_starts--;
    }
    
    for (i=0; i < NUMTHREADS * THREAD_STARTS; i++) {
        status = sem_wait(&done);
        assert(status == 0);
        printf("thread finished\n");
        if (thread_starts) {
            thread_starts--;
            newthread(demo, NULL);
        }
    }
    
    return 0;
}
