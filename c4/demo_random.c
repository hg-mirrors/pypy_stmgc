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
#define GCTID_WEAKREF         122

struct node;
typedef struct node * nodeptr;
struct weak_node {
    struct stm_object_s hdr;
    nodeptr node;
};
typedef struct weak_node * weaknodeptr;
#define WEAKNODE_SIZE  sizeof(struct weak_node)

struct node {
    struct stm_object_s hdr;
    long value;
    revision_t id;
    revision_t hash;
    nodeptr next;
    weaknodeptr weakref;
};



size_t stmcb_size(gcptr ob)
{
    if (stm_get_tid(ob) == GCTID_STRUCT_NODE)
        return sizeof(struct node);
    else if (stm_get_tid(ob) == GCTID_WEAKREF)
        return WEAKNODE_SIZE;
    assert(0);
}

void stmcb_trace(gcptr ob, void visit(gcptr *))
{
    nodeptr n;
    if (stm_get_tid(ob) == GCTID_WEAKREF)
        return;
    assert(stm_get_tid(ob) == GCTID_STRUCT_NODE);
    n = (nodeptr)ob;
    visit((gcptr *)&n->next);
    visit((gcptr *)&n->weakref);
}


// global and per-thread-data
time_t default_seed;
gcptr shared_roots[SHARED_ROOTS];

struct thread_data {
    unsigned int thread_seed;
    gcptr roots[MAXROOTS];
    gcptr roots_outside_perform[MAXROOTS];
    int num_roots;
    int num_roots_outside_perform;
    int steps_left;
    int interruptible;
    int atomic;
};
__thread struct thread_data td;


// helper functions
int classify(gcptr p);
void check(gcptr p);
int in_nursery(gcptr obj);
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

gcptr get_random_root()
{
    int num = get_rand(td.num_roots + 1);
    if (num == 0)
        return stm_thread_local_obj;
    else
        return td.roots[num - 1];
}

gcptr get_random_shared_root()
{
    int num = get_rand(SHARED_ROOTS);
    return shared_roots[num];
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

void push_roots()
{
    int i;
    for (i = 0; i < td.num_roots; i++) {
        check(td.roots[i]);
        if (td.roots[i])
            stm_push_root(td.roots[i]);
    }
}

void pop_roots()
{
    int i;
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
    push_roots();
    r = (nodeptr)stm_allocate(sizeof(struct node), GCTID_STRUCT_NODE);
    pop_roots();
    return r;
}


weaknodeptr allocate_weaknodeptr(nodeptr to)
{
    weaknodeptr w;
    push_roots(1);
    w = (weaknodeptr)stm_weakref_allocate(WEAKNODE_SIZE, GCTID_WEAKREF,
                                          (gcptr)to);
    pop_roots(1);
    return w;
}

void set_weakref(nodeptr n, nodeptr to)
{
    stm_push_root((gcptr)n);
    weaknodeptr w = allocate_weaknodeptr(to);
    n = (nodeptr)stm_pop_root();
    n = (nodeptr)stm_write_barrier((gcptr)n);
    n->weakref = w;
    dprintf(("set_weakref %p -> %p -> %p\n", n, w, to));
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
            assert(!in_nursery(id));
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
        push_roots();
        stm_push_root(p);
        stm_become_inevitable("fun");
        p = stm_pop_root();
        pop_roots();
    } 
    else if (k < 40) {
        push_roots();
        stmgc_minor_collect();
        pop_roots();
        p = NULL;
    } else if (k < 41 && DO_MAJOR_COLLECTS) {
        fprintf(stdout, "major collect\n");
        push_roots();
        stmgcpage_possibly_major_collect(1);
        pop_roots();
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

gcptr weakref_events(gcptr p, gcptr _r, gcptr _sr)
{
    nodeptr t;
    weaknodeptr w, ww;
    gcptr ptrs[] = {_r, _sr};
    
    int i = get_rand(2);
    int k = get_rand(3);
    switch (k) {
    case 0: // check weakref
        t = (nodeptr)read_barrier(ptrs[i]);
        w = t->weakref;
        if(w) {
            ww = (weaknodeptr)stm_read_barrier((gcptr)w);
            assert(stm_get_tid((gcptr)ww) == GCTID_WEAKREF);
            if (ww->node) {
                check((gcptr)ww->node);
                return (gcptr)ww->node;
            }
            else {
                t->weakref = NULL;
            }
        }
        p = NULL;
        break;
    case 1: // set weakref to something
        if (p)
            set_weakref((nodeptr)_r, (nodeptr)p);
        else
            set_weakref((nodeptr)_r, (nodeptr)get_random_root());
        p = NULL;
        break;
    case 2: // set weakref on shared roots
        set_weakref((nodeptr)_sr, (nodeptr)get_random_shared_root());
        p = NULL;
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
        w_sr->next = (nodeptr)get_random_shared_root();
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
    int k;

    _r = get_random_root();
    _sr = get_random_shared_root();

    k = get_rand(11);
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
    else if (k < 10)
        p = weakref_events(p, _r, _sr);
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
    push_roots();
    td.interruptible = 1;
    
    copy_roots(td.roots, td.roots_outside_perform, td.num_roots);
    td.num_roots_outside_perform = td.num_roots;
    
    stm_perform_transaction(NULL, interruptible_callback);
    
    td.num_roots = td.num_roots_outside_perform;
    copy_roots(td.roots_outside_perform, td.roots, td.num_roots);
    
    td.interruptible = 0;
    pop_roots();
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
    pop_roots();
    push_roots();
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
