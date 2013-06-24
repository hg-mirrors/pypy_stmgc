#include "stmimpl.h"


static int is_in_nursery(struct tx_descriptor *d, gcptr obj)
{
    return (d->nursery_base <= (char*)obj && ((char*)obj) < d->nursery_end);
}

/************************************************************/

void stmgc_init_nursery(void)
{
    struct tx_descriptor *d = thread_descriptor;

    assert(d->nursery_base == NULL);
    d->nursery_base = stm_malloc(GC_NURSERY);
    memset(d->nursery_base, 0, GC_NURSERY);
    d->nursery_end = d->nursery_base + GC_NURSERY;
    d->nursery_current = d->nursery_base;

    fprintf(stderr, "minor: nursery is at [%p to %p]\n", d->nursery_base,
            d->nursery_end);
}

void stmgc_done_nursery(void)
{
    struct tx_descriptor *d = thread_descriptor;
    assert(!stmgc_minor_collect_anything_to_do(d));
    stm_free(d->nursery_base, GC_NURSERY);

    gcptrlist_delete(&d->old_objects_to_trace);
    gcptrlist_delete(&d->public_with_young_copy);
}

static char *collect_and_allocate_size(size_t size);  /* forward */

inline static char *allocate_nursery(size_t size, int can_collect)
{
    struct tx_descriptor *d = thread_descriptor;
    char *cur = d->nursery_current;
    char *end = cur + size;
    d->nursery_current = end;
    if (end > d->nursery_end) {
        if (can_collect) {
            cur = collect_and_allocate_size(size);
        }
        else {
            d->nursery_current = cur;
            cur = NULL;
        }
    }
    return cur;
}

gcptr stm_allocate(size_t size, unsigned long tid)
{
    /* XXX inline the fast path */
    gcptr P = (gcptr)allocate_nursery(size, 1);
    assert(tid == (tid & STM_USER_TID_MASK));
    P->h_tid = tid;
    P->h_revision = stm_private_rev_num;
    P->h_original = 0;
    return P;
}

gcptr stmgc_duplicate(gcptr P)
{
    size_t size = stmcb_size(P);
    gcptr L = (gcptr)allocate_nursery(size, 0);
    if (L == NULL)
        return stmgc_duplicate_old(P);

    memcpy(L, P, size);
    L->h_tid &= ~GCFLAG_OLD;
    L->h_tid &= ~GCFLAG_HAS_ID;

    return L;
}

gcptr stmgc_duplicate_old(gcptr P)
{
    size_t size = stmcb_size(P);
    gcptr L = (gcptr)stmgcpage_malloc(size);
    memcpy(L, P, size);
    L->h_tid |= GCFLAG_OLD;

    return L;
}

/************************************************************/
/* Each object has a h_original pointer to an old copy of 
   the same object (e.g. an old revision), the "original". 
   The memory location of this old object is used as the ID 
   for this object. If h_original is NULL *and* it is an
   old object copy, it itself is the original. This invariant
   must be upheld by all code dealing with h_original.
   The original copy must never be moved again. Also, it may
   be just a stub-object.
   
   If we want the ID of an object which is still young,
   we must preallocate an old shadow-original that is used
   as the target of the young object in a minor collection.
   In this case, we set the HAS_ID flag on the young obj
   to notify minor_collect.
   This flag can be lost if the young obj is stolen. Then
   the stealing thread uses the shadow-original itself and
   minor_collect must not overwrite it again.
   Also, if there is already a backup-copy around, we use
   this instead of allocating another old object to use as 
   the shadow-original.
 */

static revision_t mangle_hash(revision_t n)
{
    /* To hash pointers in dictionaries.  Assumes that i shows some
       alignment (to 4, 8, maybe 16 bytes), so we use the following
       formula to avoid the trailing bits being always 0.
       This formula is reversible: two different values of 'i' will
       always give two different results.
    */
    return n ^ (n >> 4);
}


revision_t stm_hash(gcptr p)
{
    /* Prebuilt objects may have a specific hash stored in an extra 
       field. For now, we will simply always follow h_original and
       see, if it is a prebuilt object (XXX: maybe propagate a flag
       to all copies of a prebuilt to avoid this cache miss).
     */
    if (p->h_original) {
        if (p->h_tid & GCFLAG_PREBUILT_ORIGINAL) {
            return p->h_original;
        }
        gcptr orig = (gcptr)p->h_original;
        if ((orig->h_tid & GCFLAG_PREBUILT_ORIGINAL) && orig->h_original) {
            return orig->h_original;
        }
    }
    return stm_id(p);
}


revision_t stm_id(gcptr p)
{
    struct tx_descriptor *d = thread_descriptor;
    revision_t result;

    if (p->h_original) { /* fast path */
        if (p->h_tid & GCFLAG_PREBUILT_ORIGINAL) {
            /* h_original may contain a specific hash value,
               but in case of the prebuilt original version, 
               its memory location is the id */
            return mangle_hash((revision_t)p);
        }

        fprintf(stderr, "stm_id(%p) has orig fst: %p\n", 
                p, (gcptr)p->h_original);
        return mangle_hash(p->h_original);
    } 
    else if (!(p->h_tid & GCFLAG_PRIVATE_FROM_PROTECTED)
               && (p->h_tid & GCFLAG_OLD)) {
        /* we can be sure that p->h_original doesn't
           get set during the if and the else-if 
           
           XXX: check for priv_from_protected may not be 
           necessary. only if this func may be called on 
           another thread's young objects that are made 
           old at the same time, and we see the OLD flag 
           before h_original has been set.
        */
        fprintf(stderr, "stm_id(%p) is old, orig=0 fst: %p\n", p, p);
        return mangle_hash((revision_t)p);
    }
    

    
    spinlock_acquire(d->public_descriptor->collection_lock, 'I');
    /* old objects must have an h_original xOR be
       the original itself. 
       if some thread stole p when it was still young,
       it must have set h_original. stealing an old obj
       makes the old obj "original".
    */
    if (p->h_original) { /* maybe now? */
        result = p->h_original;
        fprintf(stderr, "stm_id(%p) has orig: %p\n", 
                p, (gcptr)p->h_original);
    }
    else if (p->h_tid & GCFLAG_OLD) {
        /* it must be this exact object */
        result = (revision_t)p;
        fprintf(stderr, "stm_id(%p) is old, orig=0: %p\n", p, p);
    }
    else {
        /* must create shadow original object or use
           backup, if exists */
        if (p->h_tid & GCFLAG_PRIVATE_FROM_PROTECTED) {
            gcptr B = (gcptr)p->h_revision;
            /* don't set HAS_ID, otherwise nursery will copy over backup */
            p->h_original = (revision_t)B;
            // B->h_tid |= GCFLAG_PUBLIC; done by CommitPrivateFromProtected
            
            result = (revision_t)B;
            fprintf(stderr, "stm_id(%p) young, pfp, use backup %p\n", 
                    p, (gcptr)p->h_original);
        }
        else {
            gcptr O = stmgc_duplicate_old(p);
            p->h_original = (revision_t)O;
            p->h_tid |= GCFLAG_HAS_ID;
            O->h_tid |= GCFLAG_PUBLIC;
            
            result = (revision_t)O;
            fprintf(stderr, "stm_id(%p) young, make shadow %p\n", p, O); 
        }
    }
    
    spinlock_release(d->public_descriptor->collection_lock);
    return mangle_hash(result);
}

revision_t stm_pointer_equal(gcptr p1, gcptr p2)
{
    /* types must be the same */
    if ((p1->h_tid & STM_USER_TID_MASK) != (p2->h_tid & STM_USER_TID_MASK))
        return 0;
    return stm_id(p1) == stm_id(p2);
}

/************************************************************/

static inline gcptr create_old_object_copy(gcptr obj)
{
    assert(!(obj->h_tid & GCFLAG_PUBLIC));
    assert(!(obj->h_tid & GCFLAG_NURSERY_MOVED));
    assert(!(obj->h_tid & GCFLAG_VISITED));
    assert(!(obj->h_tid & GCFLAG_WRITE_BARRIER));
    assert(!(obj->h_tid & GCFLAG_PREBUILT_ORIGINAL));
    assert(!(obj->h_tid & GCFLAG_OLD));

    gcptr fresh_old_copy = stmgc_duplicate_old(obj);

    fprintf(stderr, "minor: %p is copied to %p\n", obj, fresh_old_copy);
    return fresh_old_copy;
}

inline void copy_to_old_id_copy(gcptr obj, gcptr id)
{
    assert(!is_in_nursery(thread_descriptor, id));
    assert(id->h_tid & GCFLAG_OLD);

    size_t size = stmcb_size(obj);
    memcpy(id, obj, size);
    id->h_tid &= ~GCFLAG_HAS_ID;
    id->h_tid |= GCFLAG_OLD;
    fprintf(stderr, "copy_to_old_id_copy(%p -> %p)\n", obj, id);
}

static void visit_if_young(gcptr *root)
{
    gcptr obj = *root;
    gcptr fresh_old_copy;
    struct tx_descriptor *d = thread_descriptor;

    if (!is_in_nursery(d, obj)) {
        /* not a nursery object */
    }
    else {
        /* it's a nursery object.  Was it already moved? */
        if (UNLIKELY(obj->h_tid & GCFLAG_NURSERY_MOVED)) {
            /* yes.  Such an object can be a public object in the nursery
               too (such objects are always NURSERY_MOVED).  For all cases,
               we can just fix the ref. 
               Can be stolen objects or those we already moved.
            */
            *root = (gcptr)obj->h_revision;
            return;
        }

        if (obj->h_tid & GCFLAG_HAS_ID) {
            /* already has a place to go to */
            gcptr id_obj = (gcptr)obj->h_original;

            copy_to_old_id_copy(obj, id_obj);
            fresh_old_copy = id_obj;
            obj->h_tid &= ~GCFLAG_HAS_ID;
        } 
        else {
            /* make a copy of it outside */
            fresh_old_copy = create_old_object_copy(obj);
        }
        
        obj->h_tid |= GCFLAG_NURSERY_MOVED;
        obj->h_revision = (revision_t)fresh_old_copy;

        /* fix the original reference */
        *root = fresh_old_copy;

        /* add 'fresh_old_copy' to the list of objects to trace */
        gcptrlist_insert(&d->old_objects_to_trace, fresh_old_copy);
    }
}

static void mark_young_roots(struct tx_descriptor *d)
{
    gcptr *root = d->shadowstack;
    gcptr *end = *d->shadowstack_end_ref;

    /* XXX use a way to avoid walking all roots again and again */
    for (; root != end; root++) {
        visit_if_young(root);
    }
}

static void mark_private_from_protected(struct tx_descriptor *d)
{
    long i, size = d->private_from_protected.size;
    gcptr *items = d->private_from_protected.items;

    for (i = d->num_private_from_protected_known_old; i < size; i++) {
        assert(items[i]->h_tid & GCFLAG_PRIVATE_FROM_PROTECTED);
        assert(IS_POINTER(items[i]->h_revision));

        visit_if_young(&items[i]);

        stmcb_trace((gcptr)items[i]->h_revision, &visit_if_young);
    }

    d->num_private_from_protected_known_old = size;
}

static void trace_stub(struct tx_descriptor *d, gcptr S)
{
    revision_t w = ACCESS_ONCE(S->h_revision);
    if ((w & 3) != 2) {
        /* P has a ptr in h_revision, but this object is not a stub
           with a protected pointer.  It has likely been the case
           in the past, but someone made even more changes.
           Nothing to do now.
        */
        fprintf(stderr, "trace_stub: %p not a stub, ignored\n", S);
        return;
    }

    assert(S->h_tid & GCFLAG_STUB);
    if (STUB_THREAD(S) != d->public_descriptor) {
        /* Bah, it's indeed a stub but for another thread.  Nothing
           to do now.
        */
        fprintf(stderr, "trace_stub: %p stub wrong thread, ignored\n", S);
        return;
    }

    /* It's a stub for us.  It cannot be un-stubbed under our
       feet because we hold our own collection_lock.
    */
    gcptr L = (gcptr)(w - 2);
    fprintf(stderr, "trace_stub: %p stub -> %p\n", S, L);
    visit_if_young(&L);
    assert(S->h_tid & GCFLAG_STUB);
    S->h_revision = ((revision_t)L) | 2;
}

static void mark_stolen_young_stubs(struct tx_descriptor *d)
{
    long i, size = d->public_descriptor->stolen_young_stubs.size;
    gcptr *items = d->public_descriptor->stolen_young_stubs.items;

    for (i = 0; i < size; i++) {
        trace_stub(d, items[i]);
    }
    gcptrlist_clear(&d->public_descriptor->stolen_young_stubs);
}

static void mark_public_to_young(struct tx_descriptor *d)
{
    /* "public_with_young_copy" lists the public copies that may have
       a more recent (or in-progress) private or protected object that
       is young.  Note that public copies themselves are always old
       (short of a few exceptions that don't end up in this list).

       The list should only contain old public objects, but beyong that,
       be careful and ignore any strange object: it can show up because
       of aborted transactions (and then some different changes).
    */
    long i, size = d->public_with_young_copy.size;
    gcptr *items = d->public_with_young_copy.items;

    for (i = 0; i < size; i++) {
        gcptr P = items[i];
        assert(P->h_tid & GCFLAG_PUBLIC);
        assert(P->h_tid & GCFLAG_OLD);

        revision_t v = ACCESS_ONCE(P->h_revision);
        wlog_t *item;
        G2L_FIND(d->public_to_private, P, item, goto not_in_public_to_private);

        /* found P in 'public_to_private' */

        if (IS_POINTER(v)) {
            /* P is both a key in public_to_private and an outdated copy.
               We are in a case where we know the transaction will not
               be able to commit successfully.
            */
            fprintf(stderr, "public_to_young: %p was modified! abort!\n", P);
            item->val = NULL;
            AbortTransactionAfterCollect(d, ABRT_COLLECT_MINOR);
            continue;
        }

        fprintf(stderr, "public_to_young: %p -> %p in public_to_private\n",
                item->addr, item->val);
        assert(_stm_is_private(item->val));
        visit_if_young(&item->val);
        continue;

    not_in_public_to_private:
        if (!IS_POINTER(v)) {
            /* P is neither a key in public_to_private nor outdated.
               It must come from an older transaction that aborted.
               Nothing to do now.
            */
            fprintf(stderr, "public_to_young: %p ignored\n", P);
            continue;
        }

        fprintf(stderr, "public_to_young: %p -> ", P);
        trace_stub(d, (gcptr)v);
    }

    gcptrlist_clear(&d->public_with_young_copy);
}

static void visit_all_outside_objects(struct tx_descriptor *d)
{
    while (gcptrlist_size(&d->old_objects_to_trace) > 0) {
        gcptr obj = gcptrlist_pop(&d->old_objects_to_trace);

        assert(obj->h_tid & GCFLAG_OLD);
        assert(!(obj->h_tid & GCFLAG_WRITE_BARRIER));
        obj->h_tid |= GCFLAG_WRITE_BARRIER;

        stmcb_trace(obj, &visit_if_young);
    }
}

static void fix_list_of_read_objects(struct tx_descriptor *d)
{
    long i, limit = d->num_read_objects_known_old;
    gcptr *items = d->list_of_read_objects.items;
    assert(d->list_of_read_objects.size >= limit);

    if (d->active == 2) {
        /* inevitable transaction: clear the list of read objects */
        gcptrlist_clear(&d->list_of_read_objects);
    }

    for (i = d->list_of_read_objects.size - 1; i >= limit; --i) {
        gcptr obj = items[i];

        if (!is_in_nursery(d, obj)) {
            /* non-young or visited young objects are kept */
            continue;
        }
        else if (obj->h_tid & GCFLAG_NURSERY_MOVED) {
            /* visited nursery objects are kept and updated */
            items[i] = (gcptr)obj->h_revision;
            assert(!(items[i]->h_tid & GCFLAG_STUB));
            continue;
        }
        /* The listed object was not visited.  Unlist it. */
        items[i] = items[--d->list_of_read_objects.size];
    }
    d->num_read_objects_known_old = d->list_of_read_objects.size;
    fxcache_clear(&d->recent_reads_cache);
}

static void setup_minor_collect(struct tx_descriptor *d)
{
    spinlock_acquire(d->public_descriptor->collection_lock, 'M');  /*minor*/
    if (d->public_descriptor->stolen_objects.size != 0)
        stm_normalize_stolen_objects(d);
}

static void teardown_minor_collect(struct tx_descriptor *d)
{
    assert(gcptrlist_size(&d->old_objects_to_trace) == 0);
    assert(gcptrlist_size(&d->public_with_young_copy) == 0);
    assert(gcptrlist_size(&d->public_descriptor->stolen_objects) == 0);

    spinlock_release(d->public_descriptor->collection_lock);
}

static void minor_collect(struct tx_descriptor *d)
{
    fprintf(stderr, "minor collection [%p to %p]\n",
            d->nursery_base, d->nursery_end);

    /* acquire the "collection lock" first */
    setup_minor_collect(d);

    /* first do this, which asserts that some objects are private ---
       which fails if they have already been GCFLAG_NURSERY_MOVED */
    mark_public_to_young(d);

    mark_young_roots(d);

    mark_stolen_young_stubs(d);

    mark_private_from_protected(d);

    visit_all_outside_objects(d);

    fix_list_of_read_objects(d);

    /* now all surviving nursery objects have been moved out, and all
       surviving young-but-outside-the-nursery objects have been flagged
       with GCFLAG_OLD
    */
    teardown_minor_collect(d);

    /* clear the nursery */
#if defined(_GC_DEBUG) && _GC_DEBUG >= 2
    stm_free(d->nursery_base, GC_NURSERY);
    d->nursery_base = stm_malloc(GC_NURSERY);
    memset(d->nursery_base, 0, GC_NURSERY);
    d->nursery_end = d->nursery_base + GC_NURSERY;
    fprintf(stderr, "minor: nursery moved to [%p to %p]\n", d->nursery_base,
            d->nursery_end);
#else
    memset(d->nursery_base, 0, GC_NURSERY);
#endif
    d->nursery_current = d->nursery_base;

    assert(!stmgc_minor_collect_anything_to_do(d));
}

void stmgc_minor_collect(void)
{
    struct tx_descriptor *d = thread_descriptor;
    assert(d->active >= 1);
    minor_collect(d);
    AbortNowIfDelayed();
}

void stmgc_minor_collect_no_abort(void)
{
    struct tx_descriptor *d = thread_descriptor;
    minor_collect(d);
}

int stmgc_minor_collect_anything_to_do(struct tx_descriptor *d)
{
    if (d->nursery_current == d->nursery_base /*&&
        !g2l_any_entry(&d->young_objects_outside_nursery)*/ ) {
        /* there is no young object */
        assert(gcptrlist_size(&d->public_with_young_copy) == 0);
        assert(gcptrlist_size(&d->list_of_read_objects) >=
               d->num_read_objects_known_old);
        assert(gcptrlist_size(&d->private_from_protected) >=
               d->num_private_from_protected_known_old);
        d->num_read_objects_known_old =
            gcptrlist_size(&d->list_of_read_objects);
        d->num_private_from_protected_known_old =
            gcptrlist_size(&d->private_from_protected);
        return 0;
    }
    else {
        /* there are young objects */
        return 1;
    }
}

static char *collect_and_allocate_size(size_t allocate_size)
{
    stmgc_minor_collect();
    //stmgcpage_possibly_major_collect(0);

    struct tx_descriptor *d = thread_descriptor;
    assert(d->nursery_current == d->nursery_base);

    //_debug_roots(d->shadowstack, *d->shadowstack_end_ref);

    d->nursery_current = d->nursery_base + allocate_size;
    assert(d->nursery_current <= d->nursery_end);  /* XXX object too big */
    return d->nursery_base;
}
