#include "stmimpl.h"


__thread gcptr stm_thread_local_obj;


static int is_in_nursery(struct tx_descriptor *d, gcptr obj)
{
    return (d->nursery <= (char*)obj && ((char*)obj) < d->nursery_end);
}

int stmgc_is_young_in(struct tx_descriptor *d, gcptr obj)
{
    /* Check if 'obj' is a young object for 'd'.  (So if it's a young
       object for another thread, returns False.) */
    return is_in_nursery(d, obj) ||
        g2l_contains(&d->young_objects_outside_nursery, obj);
}

#ifdef _GC_DEBUG
int is_young(gcptr obj)
{
    int result = (obj->h_tid & GCFLAG_OLD) == 0;
    assert(result == stmgc_is_young_in(thread_descriptor, obj));
    return result;
}
#endif

enum protection_class_t stmgc_classify(gcptr obj)
{
    /* note that this function never returns K_OLD_PRIVATE. */
    if (obj->h_revision == stm_local_revision)
        return K_PRIVATE;
    if (is_young(obj))
        return K_PROTECTED;
    else
        return K_PUBLIC;
}

static enum protection_class_t dclassify(gcptr obj)
{
    /* for assertions only; moreover this function returns K_PRIVATE
       only for young private objects, and K_OLD_PRIVATE for old ones. */
    struct tx_descriptor *d = thread_descriptor;
    int private = (obj->h_revision == stm_local_revision);
    enum protection_class_t e;

    if (is_in_nursery(d, obj)) {
        e = private ? K_PRIVATE : K_PROTECTED;
    }
    else {
        wlog_t *entry;
        G2L_FIND(d->young_objects_outside_nursery, obj, entry,
                 goto not_found);

        if (obj->h_tid & GCFLAG_OLD)
            e = private ? K_OLD_PRIVATE : K_PUBLIC;
        else
            e = private ? K_PRIVATE : K_PROTECTED;
    }
    assert(stm_dbgmem_is_active(obj, 0));
    if (e == K_PRIVATE || e == K_PROTECTED)
        assert((obj->h_tid & GCFLAG_OLD) == 0);
    else
        assert((obj->h_tid & GCFLAG_OLD) == GCFLAG_OLD);
    if (e != K_PROTECTED)
        assert(!(obj->h_tid & GCFLAG_STOLEN));
    return e;

 not_found:
    assert(stm_dbgmem_is_active(obj, 1));
    assert(obj->h_tid & GCFLAG_OLD);
    return private ? K_OLD_PRIVATE : K_PUBLIC;
}

void stmgc_init_tls(void)
{
    struct tx_descriptor *d = thread_descriptor;
    stm_thread_local_obj = NULL;
    d->thread_local_obj_ref = &stm_thread_local_obj;

    d->nursery = stm_malloc(GC_NURSERY);
    if (!d->nursery) {
        fprintf(stderr, "Out of memory: cannot allocate nursery\n");
        abort();
    }
    memset(d->nursery, 0, GC_NURSERY);
    stm_dbgmem_not_used(d->nursery, GC_NURSERY, 1);
    d->nursery_current = d->nursery;
    d->nursery_end = d->nursery + GC_NURSERY;
}

void stmgc_done_tls(void)
{
    struct tx_descriptor *d = thread_descriptor;
    assert(d->nursery_current == d->nursery);  /* else, not empty! */
    stm_free(d->nursery, GC_NURSERY);
    gcptrlist_delete(&d->protected_with_private_copy);
    g2l_delete(&d->public_to_private);
    gcptrlist_delete(&d->private_old_pointing_to_young);
    gcptrlist_delete(&d->public_to_young);
    gcptrlist_delete(&d->stolen_objects);
    g2l_delete(&d->young_objects_outside_nursery);
    gcptrlist_delete(&d->old_objects_to_trace);
}

gcptr _stm_allocate_object_of_size_old(size_t size)
{
    gcptr p = stmgcpage_malloc(size);
    memset(p, 0, size);
    p->h_revision = stm_local_revision;
    p->h_tid = GCFLAG_OLD;
    return p;
}

static char *collect_and_allocate_size(size_t size);  /* forward */

gcptr stm_allocate_object_of_size(size_t size)
{
    struct tx_descriptor *d = thread_descriptor;
    char *cur = d->nursery_current;
    char *end = cur + size;
    d->nursery_current = end;
    if (end > d->nursery_end) {
        cur = collect_and_allocate_size(size);
    }
    stm_dbgmem_used_again(cur, size, 1);
    gcptr p = (gcptr)cur;
    p->h_revision = stm_local_revision;
    return p;
}

gcptr stmgc_duplicate(gcptr globalobj, revision_t extra_word)
{
    struct tx_descriptor *d = thread_descriptor;
    size_t size = stmcb_size(globalobj);
    size_t size_with_extra_word = size + (extra_word ? sizeof(revision_t) : 0);
    gcptr localobj;

    /* allocate 'size_with_extra_word', but no collection allowed here */
    char *cur = d->nursery_current;
    char *end = cur + size_with_extra_word;
    if (end > d->nursery_end) {
        /* uncommon case: allocate it young but outside the nursery,
           because it is full */
        d->nursery_current = d->nursery_end;
        localobj = (gcptr)stmgcpage_malloc(size_with_extra_word);
        g2l_insert(&d->young_objects_outside_nursery, localobj, NULL);
        /*mark*/
    }
    else {
        /* common case */
        stm_dbgmem_used_again(cur, size_with_extra_word, 1);
        d->nursery_current = end;
        localobj = (gcptr)cur;
    }
    if (extra_word)
        *(revision_t *)(((char*)localobj) + size) = extra_word;

    memcpy(localobj, globalobj, size);

    assert(!(localobj->h_tid & GCFLAG_PRIVATE_COPY));
    assert(!(localobj->h_tid & GCFLAG_NURSERY_MOVED));
    localobj->h_tid &= ~(GCFLAG_VISITED           |
                         GCFLAG_PUBLIC_TO_PRIVATE |
                         GCFLAG_PREBUILT_ORIGINAL |
                         GCFLAG_WRITE_BARRIER     |
                         GCFLAG_OLD);
    localobj->h_tid |= GCFLAG_PRIVATE_COPY;
    localobj->h_revision = stm_local_revision;
    return localobj;
}

static revision_t fetch_extra_word(gcptr L)
{
    size_t size = stmcb_size(L);
    revision_t extra_word = *(revision_t *)(((char*)L) + size);
    assert(extra_word & 1);   /* "is not a pointer", should be a rev number */
    return extra_word;
}

void stmgc_start_transaction(struct tx_descriptor *d)
{
    assert(!gcptrlist_size(&d->protected_with_private_copy));
    assert(!g2l_any_entry(&d->public_to_private));
    assert(!gcptrlist_size(&d->private_old_pointing_to_young));
    d->num_read_objects_known_old = 0;
    d->num_public_to_protected = gcptrlist_size(&d->public_to_young);
}

static void fix_new_public_to_protected_references(struct tx_descriptor *d);
static void normalize_stolen_objects(struct tx_descriptor *d);

void stmgc_stop_transaction(struct tx_descriptor *d)
{
#ifdef _GC_DEBUG_NO_NURSERY_BETWEEN_TRANSACTIONS
    stmgc_minor_collect();
#endif
    if (gcptrlist_size(&d->private_old_pointing_to_young) > 0)
        fix_new_public_to_protected_references(d);

    spinlock_acquire(d->collection_lock);
    d->collection_lock = 'C';   /* Committing */

    if (gcptrlist_size(&d->stolen_objects) > 0)
        normalize_stolen_objects(d);

    fprintf(stderr, "stop transaction\n");
}

void stmgc_committed_transaction(struct tx_descriptor *d)
{
    spinlock_release(d->collection_lock);
}

void stmgc_abort_transaction(struct tx_descriptor *d)
{
    /* cancel the change to the h_revision done in LocalizeProtected() */
    long i, size = d->protected_with_private_copy.size;
    gcptr *items = d->protected_with_private_copy.items;

    /* Note that the thrown-away private objects are kept around.  It
       may possibly be a small optimization to reuse the part of the
       nursery that contains them.  To be on the safe "future-proof"
       side, we grab the collection lock here, to make sure that no
       other thread is busy stealing (which includes reading the extra
       word immediately after private objects).
    */
    if (d->collection_lock != 'C')
        spinlock_acquire(d->collection_lock);

    for (i = 0; i < size; i++) {
        gcptr R = items[i];
        assert(dclassify(R) == K_PROTECTED);
        assert(!(R->h_revision & 1));    /* "is a pointer" */
        gcptr L = (gcptr)R->h_revision;

        if (R->h_tid & GCFLAG_STOLEN) {    /* ignore stolen objects */
            assert(dclassify(L) == K_PUBLIC);
            continue;
        }
        assert(dclassify(L) == K_PRIVATE);

        R->h_revision = fetch_extra_word(L);
        abort();//XXX
    }
    gcptrlist_clear(&d->protected_with_private_copy);
    spinlock_release(d->collection_lock);

    g2l_clear(&d->public_to_private);
    gcptrlist_clear(&d->private_old_pointing_to_young);
    d->public_to_young.size = d->num_public_to_protected;
}

/************************************************************/


extern void recdump(gcptr obj);    /* in gcpage.c */
static void recdump1(char *msg, gcptr obj)
{
    fprintf(stderr, "\n<--------------------%s--------------------> ", msg);
    recdump(obj);
}
#ifdef DUMP_EXTRA
#  define _REASON(x)          , x
#  define _REASON_STMT(x)     x
#else
#  define _REASON(x)          /* removed */
#  define _REASON_STMT(x)     /* removed */
#  define recdump1(msg, obj)  /* removed */
#endif

static inline gcptr create_old_object_copy(gcptr obj  _REASON(char *reason))
{
    assert(!(obj->h_tid & GCFLAG_NURSERY_MOVED));
    assert(!(obj->h_tid & GCFLAG_VISITED));
    assert(!(obj->h_tid & GCFLAG_WRITE_BARRIER));
    assert(!(obj->h_tid & GCFLAG_PUBLIC_TO_PRIVATE));
    assert(!(obj->h_tid & GCFLAG_PREBUILT_ORIGINAL));
    assert(!(obj->h_tid & GCFLAG_OLD));

    size_t size = stmcb_size(obj);
    gcptr fresh_old_copy = stmgcpage_malloc(size);
    memcpy(fresh_old_copy, obj, size);
    fresh_old_copy->h_tid |= GCFLAG_OLD;

#ifdef DUMP_EXTRA
    fprintf(stderr, "%s: %p is copied to %p\n", reason, obj, fresh_old_copy);
#endif
    return fresh_old_copy;
}


#ifdef DUMP_EXTRA
#  define PATCH_ROOT_WITH(obj)                                          \
              fprintf(stderr, "(patch at %p <- %p)\n", root, (obj));    \
              *root = (obj)
#else
#  define PATCH_ROOT_WITH(obj)   *root = (obj)
#endif

static void visit_if_young(gcptr *root  _REASON(char *reason))
{
    gcptr obj = *root;
    gcptr fresh_old_copy;
    gcptr previous_obj = NULL;
    struct tx_descriptor *d = thread_descriptor;

    recdump1(reason, obj);

    /* Note: it's a good idea here to avoid reading any field of 'obj'
       before we know it's a young object.  This avoids a lot of cache
       misses and cache pollution.
    */
 retry:
    if (!is_in_nursery(d, obj)) {
        /* 'obj' is not from the nursery (or 'obj == NULL') */
        if (obj == NULL || !g2l_contains(
                               &d->young_objects_outside_nursery, obj)) {
            return;   /* then it's an old object or NULL, nothing to do */
        }
        /* it's a young object outside the nursery */

        /* is it a protected object with a more recent revision?
           (this test fails automatically if it's a private object) */
        if (!(obj->h_revision & 1)) {
            goto ignore_and_try_again_with_next;
        }
        /* was it already marked? */
        if (obj->h_tid & GCFLAG_OLD) {
            return;    /* yes, and no '*root' to fix, as it doesn't move */
        }
        /* otherwise, add GCFLAG_OLD, and continue below */
        obj->h_tid |= GCFLAG_OLD;
        fresh_old_copy = obj;
    }
    else {
        /* it's a nursery object.  Is it:
           A. an already-moved nursery object?
           B. a protected object with a more recent revision?
           C. common case: first visit to an object to copy outside
        */
        if (!(obj->h_revision & 1)) {
            
            if (obj->h_tid & GCFLAG_NURSERY_MOVED) {
                /* case A: just fix the ref. */
                PATCH_ROOT_WITH((gcptr)obj->h_revision);
                return;
            }
            else {
                /* case B */
                goto ignore_and_try_again_with_next;
            }
        }
        /* case C */
        fresh_old_copy = create_old_object_copy(obj  _REASON("visit"));
        obj->h_tid |= GCFLAG_NURSERY_MOVED;
        obj->h_revision = (revision_t)fresh_old_copy;

        /* fix the original reference */
        PATCH_ROOT_WITH(fresh_old_copy);
    }

    /* add 'fresh_old_copy' to the list of objects to trace */
    assert(!(fresh_old_copy->h_tid & GCFLAG_WRITE_BARRIER));
    gcptrlist_insert(&d->old_objects_to_trace, fresh_old_copy);
    recdump1("MOVED TO", fresh_old_copy);
    return;

 ignore_and_try_again_with_next:
    if (previous_obj == NULL) {
        previous_obj = obj;
    }
    else {
        previous_obj->h_revision = obj->h_revision;    /* compress chain */
        previous_obj = NULL;
    }
    obj = (gcptr)obj->h_revision;
    assert(stmgc_classify(obj) != K_PRIVATE);
    PATCH_ROOT_WITH(obj);
    goto retry;
}
#undef PATCH_ROOT_WITH

static gcptr young_object_becomes_old(struct tx_descriptor *d, gcptr obj
                                      _REASON(char *reason))
{
    assert(stmgc_is_young_in(d, obj));
    assert(!(obj->h_tid & GCFLAG_NURSERY_MOVED));
    assert(!(obj->h_tid & GCFLAG_VISITED));
    assert(!(obj->h_tid & GCFLAG_WRITE_BARRIER));
    assert(!(obj->h_tid & GCFLAG_PUBLIC_TO_PRIVATE));
    assert(!(obj->h_tid & GCFLAG_PREBUILT_ORIGINAL));
    assert(!(obj->h_tid & GCFLAG_OLD));
    assert(obj->h_revision & 1);    /* odd value so far */

    visit_if_young(&obj  _REASON(reason));
    return obj;
}

static void mark_young_roots(gcptr *root, gcptr *end)
{
    /* XXX use a way to avoid walking all roots again and again */
    for (; root != end; root++) {
        visit_if_young(root  _REASON("ROOT"));
    }
}

static void _debug_roots(gcptr *root, gcptr *end)
{
#ifdef DUMP_EXTRA
    for (; root != end; root++) {
        recdump1("ROOT END", *root);
    }
#endif
}

static void mark_protected_with_private_copy(struct tx_descriptor *d)
{
    long i, size = d->protected_with_private_copy.size;
    gcptr *items = d->protected_with_private_copy.items;

    /* the young objects listed in 'protected_with_private_copy' need
       various adjustements:
    */
    for (i = 0; i < size; i++) {
        gcptr R = items[i];
        assert(dclassify(R) == K_PROTECTED);
        assert(!(R->h_revision & 1));    /* "is a pointer" */
        gcptr L = (gcptr)R->h_revision;

        if (R->h_tid & GCFLAG_STOLEN) {    /* ignore stolen objects */
            assert(dclassify(L) == K_PUBLIC);
            continue;
        }
        assert(dclassify(L) == K_PRIVATE);

        /* we first detach the link, restoring the original R->h_revision */
        R->h_revision = fetch_extra_word(L);

        /* then we turn the protected object in a public one */
        R = young_object_becomes_old(d, R
                    _REASON("PROTECTED pointing to private"));
        assert(dclassify(R) == K_PUBLIC);

        /* adjust flags on the newly public object */
        R->h_tid |= GCFLAG_PUBLIC_TO_PRIVATE;

        /* then we fix the target private object, which is necessarily young */
        L = young_object_becomes_old(d, L
                    _REASON("PRIVATE COPY from protected"));
        assert(dclassify(L) == K_OLD_PRIVATE);

        /* then we record the dependency in the dictionary
           'public_to_private' */
        g2l_insert(&d->public_to_private, R, L);
        /*mark*/
    }

    gcptrlist_clear(&d->protected_with_private_copy);
}

static void mark_public_to_young(struct tx_descriptor *d)
{
    long i, size = d->public_to_young.size;
    gcptr *items = d->public_to_young.items;
    long intermediate_limit = d->num_public_to_protected;

    /* the slice public_to_young[:num_public_to_protected] lists public
       objects that have an 'h_revision' pointer going to a protected
       object.  These pointers have a value with bit 2 set. */

    for (i = 0; i < intermediate_limit; i++) {
        gcptr R = items[i];
        gcptr L;
        revision_t v = R->h_revision;
        assert(dclassify(R) == K_PUBLIC);
        assert(!(v & 1));   /* "is a pointer" */

        if (v & 2) {        /* a pointer with bit 2 set.
                               Normally set, except if R was stolen */
            L = (gcptr)(v & ~2);
            assert(dclassify(L) == K_PROTECTED);
            visit_if_young(&L  _REASON("public.h_revision -> PROTECTED"));
            /* The new value of L is the previously-protected object moved
               outside.  We can't store it immediately in R->h_revision!
               We have to wait until the end of the minor collection.  See
               finish_public_to_young(). */
        }
        /*mark*/
    }

    /* the slice public_to_young[num_public_to_protected:] lists public
       objects that have an entry in 'public_to_private', which points
       to a young private object. */

    for (; i < size; i++) {
        gcptr R = items[i];
        assert(dclassify(R) == K_PUBLIC);

        wlog_t *entry;
        G2L_FIND(d->public_to_private, R, entry, goto not_found);
        assert(R == entry->addr);
        gcptr L = entry->val;
        assert(dclassify(L) == K_PRIVATE);
        L = young_object_becomes_old(d, L
                    _REASON("PRIVATE COPY from public"));
        assert(dclassify(L) == K_OLD_PRIVATE);
        entry->val = L;
        /*mark*/
    }
    return;

 not_found:
    assert(!"not found in public_to_private");
    abort();   /* should not occur */
}

static void finish_public_to_young(struct tx_descriptor *d)
{
    gcptr *items = d->public_to_young.items;
    long i, intermediate_limit = d->num_public_to_protected;

    /* first, we issue a memory write barrier: it ensures that all the newly
       public object we wrote are really written in memory _before_ we change
       the h_revision fields of the pre-existing objects to point to them. */
    smp_wmb();

    for (i = 0; i < intermediate_limit; i++) {
        gcptr R = items[i];
        gcptr L;
        revision_t v = R->h_revision;
        assert(dclassify(R) == K_PUBLIC);
        assert(!(v & 1));   /* "is a pointer" */
        if (v & 2) {        /* a pointer with bit 2 set.
                               Normally set, except if R was stolen */
            L = (gcptr)(v & ~2);

            /* use visit_if_young() again to find the final newly-public
               object */
            visit_if_young(&L  _REASON("public.h_revision -> FETCH PUBLIC"));
            assert(dclassify(L) == K_PUBLIC);

            /* Note that although R is public, its h_revision cannot be
               modified under our feet as long as we hold the collection lock,
               because it's pointing to one of our protected objects */
            R->h_revision = (revision_t)L;
        }
        /*mark*/
    }

    gcptrlist_clear(&d->public_to_young);
    d->num_public_to_protected = 0;
}

static void mark_private_old_pointing_to_young(struct tx_descriptor *d)
{
    /* trace the objects recorded earlier by stmgc_write_barrier() */
    gcptrlist_move(&d->old_objects_to_trace,
                   &d->private_old_pointing_to_young);
}


#ifdef DUMP_EXTRA
static void visit_if_young_1(gcptr *root)
{
    visit_if_young(root, "VISIT");
}
#else
#  define visit_if_young_1  visit_if_young
#endif

static void visit_all_outside_objects(struct tx_descriptor *d)
{
    while (gcptrlist_size(&d->old_objects_to_trace) > 0) {
        gcptr obj = gcptrlist_pop(&d->old_objects_to_trace);

        assert(dclassify(obj) == K_PUBLIC || dclassify(obj) == K_OLD_PRIVATE);
        assert(!(obj->h_tid & GCFLAG_WRITE_BARRIER));
        obj->h_tid |= GCFLAG_WRITE_BARRIER;

        stmcb_trace(obj, &visit_if_young_1);
    }
}

static void fix_list_of_read_objects(struct tx_descriptor *d)
{
    long i, limit = d->num_read_objects_known_old;
    gcptr *items = d->list_of_read_objects.items;
    assert(d->list_of_read_objects.size >= limit);

    for (i = d->list_of_read_objects.size - 1; i >= limit; --i) {
        gcptr obj = items[i];

        if (!is_in_nursery(d, obj)) {
            if (!g2l_contains(&d->young_objects_outside_nursery, obj) ||
                (obj->h_tid & GCFLAG_OLD)) {
                /* non-young or visited young objects are kept (the
                   first line of this check could be removed, but it is
                   better this way to avoid cache pollution) */
                continue;
            }
        }
        else if (obj->h_tid & GCFLAG_NURSERY_MOVED) {
            /* visited nursery objects are kept and updated */
            items[i] = (gcptr)obj->h_revision;
            /*mark*/
            continue;
        }
        /* The listed object was not visited.  Either it's because it
           because really unreachable (in which case it cannot possibly
           be modified any more, and the current transaction cannot
           abort because of it) or it's because it was already modified.
        */
        if (obj->h_revision & 1) {
            /* first case: untrack it */
            abort();//XXX
            items[i] = items[--d->list_of_read_objects.size];
        }
        else {
            /* second case */
            abort();//XXX
            /* ABRT_COLLECT_MINOR ... check
               for stolen object */
        }
    }
    d->num_read_objects_known_old = d->list_of_read_objects.size;
    fxcache_clear(&d->recent_reads_cache);
}

static void setup_minor_collect(struct tx_descriptor *d)
{
    spinlock_acquire(d->collection_lock);
    assert(gcptrlist_size(&d->old_objects_to_trace) == 0);
}

static void teardown_minor_collect(struct tx_descriptor *d)
{
    assert(gcptrlist_size(&d->old_objects_to_trace) == 0);
    assert(!g2l_any_entry(&d->young_objects_outside_nursery));
    assert(d->num_read_objects_known_old ==
               gcptrlist_size(&d->list_of_read_objects));

    assert(gcptrlist_size(&d->protected_with_private_copy) == 0);
    assert(gcptrlist_size(&d->private_old_pointing_to_young) == 0);
    assert(gcptrlist_size(&d->public_to_young) == 0);
    assert(d->num_public_to_protected == 0);
    assert(gcptrlist_size(&d->stolen_objects) == 0);

    spinlock_release(d->collection_lock);
}

static void create_yo_stubs(gcptr *pobj)
{
    gcptr obj = *pobj;
    if (obj == NULL)
        return;

    struct tx_descriptor *d = thread_descriptor;
    if (!stmgc_is_young_in(d, obj))
        return;

    /* xxx try to avoid duplicate stubs for the same object */
    gcptr stub = stmgcpage_malloc(sizeof(*stub));
    stub->h_tid = 0;   /* no flags */
    stub->h_revision = ((revision_t)obj) | 2;
    *pobj = stub;
    abort();//XXX
}

static void fix_new_public_to_protected_references(struct tx_descriptor *d)
{
    long i, size = d->private_old_pointing_to_young.size;
    gcptr *items = d->private_old_pointing_to_young.items;

    for (i = 0; i < size; i++) {
        stmcb_trace(items[i], &create_yo_stubs);
    }
    gcptrlist_clear(&d->private_old_pointing_to_young);
}

static void free_unvisited_young_objects_outside_nursery(
                                            struct tx_descriptor *d)
{
    wlog_t *item;

    G2L_LOOP_FORWARD(d->young_objects_outside_nursery, item) {

        gcptr obj = item->addr;
        if (!(obj->h_tid & GCFLAG_OLD)) {
            /* dies */
            stmgcpage_free(obj);
        }

    } G2L_LOOP_END;

    g2l_clear(&d->young_objects_outside_nursery);
}

static void minor_collect(struct tx_descriptor *d)
{
    fprintf(stderr, "minor collection [%p - %p]\n",
            d->nursery, d->nursery_end);

    /* acquire the "collection lock" first */
    setup_minor_collect(d);

    if (gcptrlist_size(&d->stolen_objects) > 0)
        normalize_stolen_objects(d);

    mark_protected_with_private_copy(d);

    mark_public_to_young(d);

    mark_young_roots(d->shadowstack, *d->shadowstack_end_ref);

    mark_private_old_pointing_to_young(d);

    visit_all_outside_objects(d);
    fix_list_of_read_objects(d);

    /* now all surviving nursery objects have been moved out, and all
       surviving young-but-outside-the-nursery objects have been flagged
       with GCFLAG_OLD */
    finish_public_to_young(d);

    if (g2l_any_entry(&d->young_objects_outside_nursery))
        free_unvisited_young_objects_outside_nursery(d);

    teardown_minor_collect(d);

    /* clear the nursery */
    stm_dbgmem_used_again(d->nursery, GC_NURSERY, 1);
    memset(d->nursery, 0, GC_NURSERY);
    stm_dbgmem_not_used(d->nursery, GC_NURSERY, 1);

    d->nursery_current = d->nursery;

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
    if (d->nursery_current == d->nursery &&
        !g2l_any_entry(&d->young_objects_outside_nursery)) {
        /* there is no young object */
        assert(gcptrlist_size(&d->private_old_pointing_to_young) == 0);
        assert(gcptrlist_size(&d->public_to_young) == 0);
        assert(gcptrlist_size(&d->stolen_objects) == 0);
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
    stmgcpage_possibly_major_collect(0);

    struct tx_descriptor *d = thread_descriptor;
    assert(d->nursery_current == d->nursery);

    _debug_roots(d->shadowstack, *d->shadowstack_end_ref);

    assert(allocate_size <= GC_NURSERY);   /* XXX */
    d->nursery_current = d->nursery + allocate_size;
    return d->nursery;
}

void stmgc_write_barrier(gcptr obj)
{
    struct tx_descriptor *d = thread_descriptor;

#ifdef DUMP_EXTRA
    fprintf(stderr, "stmgc_write_barrier: %p\n", obj);
#endif
    assert(dclassify(obj) == K_OLD_PRIVATE);
    assert(obj->h_tid & GCFLAG_WRITE_BARRIER);
    obj->h_tid &= ~GCFLAG_WRITE_BARRIER;
    gcptrlist_insert(&d->private_old_pointing_to_young, obj);
}

int stmgc_nursery_hiding(int hide)
{
#ifdef _GC_DEBUG
    struct tx_descriptor *d = thread_descriptor;
    if (hide) {
        stm_dbgmem_not_used(d->nursery, GC_NURSERY, 1);
    }
    else {
        stm_dbgmem_used_again(d->nursery,
                              d->nursery_current - d->nursery, 1);
    }

    wlog_t *item;

    G2L_LOOP_FORWARD(d->young_objects_outside_nursery, item) {

        gcptr obj = item->addr;
        if (hide) {
            size_t size = stmcb_size(obj);
            stm_dbgmem_not_used(obj, size, 0);
        }
        else {
            stm_dbgmem_used_again(obj, sizeof(gcptr *), 0);
            size_t size = stmcb_size(obj);
            stm_dbgmem_used_again(obj, size, 0);
        }

    } G2L_LOOP_END;
#endif
    return 1;
}

/************************************************************/

static gcptr extract_from_foreign_nursery(gcptr R)
{
    /* "Stealing": this function follows a chain of protected objects in
       the foreign nursery of the thread temporarily in
       'thread_descriptor'.  It copies the last one outside the nursery,
       and return it. */
    gcptr R2, N;
    revision_t source_local_rev, v;

    source_local_rev = stm_local_revision;
    v = ACCESS_ONCE(R->h_revision);

    /* check that R is a protected object */
    assert(!(R->h_tid & GCFLAG_OLD));
    assert(v != source_local_rev);

    /* walk to the head of the chain in the foreign nursery
     */
    while (!(v & 1)) {     /* "is a pointer" */
        R2 = (gcptr)v;
        v = ACCESS_ONCE(R2->h_revision);
        if (v == source_local_rev) {
            /* R->h_revision is a pointer, but the target is a private
               object.  We ignore private objects, so we stay at R; but
               have to fetch R's real revision off-line from the extra
               word that follows R2 */
            v = fetch_extra_word(R2);
            break;
        }
        else if (R2->h_tid & GCFLAG_OLD) {
            /* we find a public object again: easy case, just return it */
            return R2;
        }
        else {
            /* the chain continues with another protected object, go on */
            R = R2;
        }
    }

    /* R is now the protected object to move outside, with revision v. */
    N = create_old_object_copy(R  _REASON("stolen copy"));
    N->h_revision = v;
    gcptrlist_insert2(&thread_descriptor->stolen_objects, R, N);

    /* there might be references in N going to protected objects.  We
       must fix them with stubs. */
    stmcb_trace(N, &create_yo_stubs);

    return N;
}

void stmgc_public_to_foreign_protected(gcptr P)
{
    /* P is a public object, which contains in h_revision a pointer to a
       protected object --- but it is protectd by another thread,
       i.e. it likely lives in a foreign nursery.  We have to copy the
       object out ourselves.  This is necessary: we can't simply wait
       for the other thread to do a minor collection, because it might
       be blocked in a system call or whatever. */
    struct tx_descriptor *my_d = thread_descriptor;
    revision_t my_local_rev = stm_local_revision;

    /* repeat the checks in the caller, to avoid passing more than one
       argument here */
    revision_t v = ACCESS_ONCE(P->h_revision);
    assert(!(v & 1));    /* "is a pointer" */
    if (!(v & 2))
        return;   /* changed already, retry */

    gcptr R = (gcptr)(v & ~2);

    /* We need to look up which thread it belongs to and lock this
       thread's minor collection lock.  This also prevents several
       threads from getting on each other's toes trying to extract
       objects from the same nursery */
    struct tx_descriptor *source_d = stm_find_thread_containing_pointer(R);
    assert(source_d != my_d);

    spinlock_acquire(source_d->collection_lock);

    /* now that we have the lock, check again that P->h_revision was not
       modified in the meantime.  If it did change, we do nothing and will
       retry.
    */
    if (P->h_revision == v) {
        /* temporarily take the identity of source_d */
        thread_descriptor = source_d;
        stm_local_revision = *source_d->local_revision_ref;

        /* debugging support: "activate" the foreign nursery */
        int was_active = stm_dbgmem_is_active(source_d->nursery, 0);
        if (!was_active) assert(stmgc_nursery_hiding(0));

        /* copy the protected source object */
        gcptr N = extract_from_foreign_nursery(R);

        /* make sure the copy N is visible to other threads before we
           change P->h_revision */
        smp_wmb();

        /* do the change to P->h_revision */
        ACCESS_ONCE(P->h_revision) = (revision_t)N;
        fprintf(stderr, "STEALING: %p->h_revision changed from %p to %p\n",
                P, R, N);

        /* debugging support: "deactivate" the foreign nursery again */
        if (!was_active) assert(stmgc_nursery_hiding(1));

        /* restore my own identity */
        stm_local_revision = my_local_rev;
        thread_descriptor = my_d;
    }
    spinlock_release(source_d->collection_lock);
}

static void normalize_stolen_objects(struct tx_descriptor *d)
{
    /* d->stolen_objects lists pairs (R, N) with R being a protected
       object, and N a public object at the _same_ revision (and with
       identical content).  The protected object R can have a private
       copy, but it cannot have another already-committed 'h_revision'.
    */
    long i, size = d->stolen_objects.size;
    gcptr *items = d->stolen_objects.items;

    for (i = 0; i < size; i += 2) {
        gcptr R = items[i];
        gcptr N = items[i + 1];

        assert(dclassify(R) == K_PROTECTED);
        assert(dclassify(N) == K_PUBLIC);

        revision_t v = R->h_revision;
        R->h_tid |= GCFLAG_STOLEN;
        R->h_revision = (revision_t)N;

        if (!(v & 1)) {   /* "is a pointer" */
            gcptr L = (gcptr)v;
            assert(dclassify(L) == K_PRIVATE);

            /* R has got a private copy L.  This means that R is listed
               in protected_with_private_copy.  Where?  We don't know.
               The other places that scan protected_with_private_copy
               must carefully ignore GCFLAG_STOLEN entries. */

            /* we re-insert L as a private copy of the public object N */
            N->h_tid |= GCFLAG_PUBLIC_TO_PRIVATE;
            g2l_insert(&d->public_to_private, N, L);
            gcptrlist_insert(&d->public_to_young, N);
        }
    }
    gcptrlist_clear(&d->stolen_objects);
    abort();//XXX
}

void stmgc_normalize_stolen_objects(void)
{
    struct tx_descriptor *d = thread_descriptor;
    spinlock_acquire(d->collection_lock);
    normalize_stolen_objects(d);
    spinlock_release(d->collection_lock);
}
