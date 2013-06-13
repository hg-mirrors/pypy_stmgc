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
}

void stmgc_done_nursery(void)
{
    struct tx_descriptor *d = thread_descriptor;
    assert(!stmgc_minor_collect_anything_to_do(d));
    stm_free(d->nursery_base, GC_NURSERY);

    gcptrlist_delete(&d->old_objects_to_trace);
}

static char *collect_and_allocate_size(size_t size);  /* forward */

gcptr stm_allocate(size_t size, unsigned long tid)
{
    /* XXX inline the fast path */
    struct tx_descriptor *d = thread_descriptor;
    char *cur = d->nursery_current;
    char *end = cur + size;
    d->nursery_current = end;
    if (end > d->nursery_end) {
        cur = collect_and_allocate_size(size);
    }
    gcptr P = (gcptr)cur;
    assert(tid == (tid & STM_USER_TID_MASK));
    P->h_tid = tid;
    P->h_revision = stm_private_rev_num;
    return P;
}

/************************************************************/

static void visit_if_young(gcptr *root)
{
    gcptr obj = *root;
    gcptr fresh_old_copy;
    struct tx_descriptor *d = thread_descriptor;

    if (!is_in_nursery(d, obj)) {
        /* not a nursery object */
    }
    else {
        /* a nursery object */
        assert(!(obj->h_tid & GCFLAG_WRITE_BARRIER));
        assert(!(obj->h_tid & GCFLAG_OLD));
        assert(!(obj->h_tid & GCFLAG_PREBUILT_ORIGINAL));

        /* make a copy of it outside */
        fresh_old_copy = stmgc_duplicate(obj);
        obj->h_tid |= GCFLAG_NURSERY_MOVED;
        obj->h_revision = (revision_t)fresh_old_copy;

        /* fix the original reference */
        *root = fresh_old_copy;

        /* add 'fresh_old_copy' to the list of objects to trace */
        gcptrlist_insert(&d->old_objects_to_trace, fresh_old_copy);
    }
}

static void mark_young_roots(gcptr *root, gcptr *end)
{
    /* XXX use a way to avoid walking all roots again and again */
    for (; root != end; root++) {
        visit_if_young(root);
    }
}

static void visit_all_outside_objects(struct tx_descriptor *d)
{
    while (gcptrlist_size(&d->old_objects_to_trace) > 0) {
        gcptr obj = gcptrlist_pop(&d->old_objects_to_trace);

        assert(!(obj->h_tid & GCFLAG_OLD));
        assert(!(obj->h_tid & GCFLAG_WRITE_BARRIER));
        obj->h_tid |= GCFLAG_OLD | GCFLAG_WRITE_BARRIER;

        stmcb_trace(obj, &visit_if_young);
    }
}

static void setup_minor_collect(struct tx_descriptor *d)
{
    spinlock_acquire(d->public_descriptor->collection_lock, 'M');  /*minor*/
    assert(gcptrlist_size(&d->old_objects_to_trace) == 0);

    if (d->public_descriptor->stolen_objects.size != 0)
        stm_normalize_stolen_objects(d);
}

static void teardown_minor_collect(struct tx_descriptor *d)
{
    assert(gcptrlist_size(&d->old_objects_to_trace) == 0);
    assert(gcptrlist_size(&d->public_descriptor->stolen_objects) == 0);

    spinlock_release(d->public_descriptor->collection_lock);
}

static void minor_collect(struct tx_descriptor *d)
{
    fprintf(stderr, "minor collection [%p to %p]\n",
            d->nursery_base, d->nursery_end);

    /* acquire the "collection lock" first */
    setup_minor_collect(d);

    mark_young_roots(d->shadowstack, *d->shadowstack_end_ref);

#if 0
    mark_private_from_protected(d);

    mark_public_to_young(d);

    mark_private_old_pointing_to_young(d);
#endif

    visit_all_outside_objects(d);
#if 0
    fix_list_of_read_objects(d);

    /* now all surviving nursery objects have been moved out, and all
       surviving young-but-outside-the-nursery objects have been flagged
       with GCFLAG_OLD */
    finish_public_to_young(d);

    if (g2l_any_entry(&d->young_objects_outside_nursery))
        free_unvisited_young_objects_outside_nursery(d);
#endif

    teardown_minor_collect(d);

    /* clear the nursery */
    memset(d->nursery_base, 0, GC_NURSERY);
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

int stmgc_minor_collect_anything_to_do(struct tx_descriptor *d)
{
    if (d->nursery_current == d->nursery_base /*&&
        !g2l_any_entry(&d->young_objects_outside_nursery)*/ ) {
        /* there is no young object */
        //assert(gcptrlist_size(&d->private_old_pointing_to_young) == 0);
        //assert(gcptrlist_size(&d->public_to_young) == 0);
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
