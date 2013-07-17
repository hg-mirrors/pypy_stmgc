#include "stmimpl.h"

#define WEAKREF_PTR(wr, sz)  (*(gcptr *)(((char *)(wr)) + (sz) - WORD))


gcptr stm_weakref_allocate(size_t size, unsigned long tid, gcptr obj)
{
    gcptr weakref = stm_allocate(size, tid);
    assert(!(weakref->h_tid & GCFLAG_OLD));   /* 'size' too big? */
    assert(stmgc_size(weakref) == size);
    WEAKREF_PTR(weakref, size) = obj;
    gcptrlist_insert(&thread_descriptor->young_weakrefs, weakref);
    return weakref;
}


/***** Minor collection *****/

static int is_in_nursery(struct tx_descriptor *d, gcptr obj)
{
    return (d->nursery_base <= (char*)obj && ((char*)obj) < d->nursery_end);
}

void stm_move_young_weakrefs(struct tx_descriptor *d)
{
    /* The code relies on the fact that no weakref can be an old object
       weakly pointing to a young object.  Indeed, weakrefs are immutable
       so they cannot point to an object that was created after it.
    */
    while (gcptrlist_size(&d->young_weakrefs) > 0) {
        gcptr weakref = gcptrlist_pop(&d->young_weakrefs);
        if (!(weakref->h_tid & GCFLAG_NURSERY_MOVED))
            continue;   /* the weakref itself dies */

        weakref = (gcptr)weakref->h_revision;
        size_t size = stmgc_size(weakref);
        gcptr pointing_to = WEAKREF_PTR(weakref, size);
        assert(pointing_to != NULL);

        if (is_in_nursery(d, pointing_to)) {
            if (pointing_to->h_tid & GCFLAG_NURSERY_MOVED) {
                WEAKREF_PTR(weakref, size) = (gcptr)pointing_to->h_revision;
            }
            else {
                WEAKREF_PTR(weakref, size) = NULL;
                continue;   /* no need to remember this weakref any longer */
            }
        }
        else {
            /*  # see test_weakref_to_prebuilt: it's not useful to put
                # weakrefs into 'old_objects_with_weakrefs' if they point
                # to a prebuilt object (they are immortal).  If moreover
                # the 'pointing_to' prebuilt object still has the
                # GCFLAG_NO_HEAP_PTRS flag, then it's even wrong, because
                # 'pointing_to' will not get the GCFLAG_VISITED during
                # the next major collection.  Solve this by not registering
                # the weakref into 'old_objects_with_weakrefs'.
            */
        }
        gcptrlist_insert(&d->public_descriptor->old_weakrefs, weakref);
    }
}


/***** Major collection *****/

void stm_invalidate_old_weakrefs(struct tx_public_descriptor *gcp)
{
    /* walk over list of objects that contain weakrefs.  If the
       object it references does not survive, invalidate the weakref */
    long i;
    gcptr *items = gcp->old_weakrefs.items;

    for (i = gcp->old_weakrefs.size - 1; i >= 0; i--) {
        gcptr weakref = items[i];

        if (!(weakref->h_tid & GCFLAG_VISITED)) {
            /* weakref itself dies */
        }
        else {
            size_t size = stmgc_size(weakref);
            gcptr pointing_to = WEAKREF_PTR(weakref, size);
            //...;
            abort();
        }

        /* remove this weakref from the list */
        items[i] = items[--gcp->old_weakrefs.size];
    }

    gcptrlist_compress(&gcp->old_weakrefs);
}
