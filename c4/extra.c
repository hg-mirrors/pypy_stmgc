#include "stmimpl.h"


void stm_copy_to_old_id_copy(gcptr obj, gcptr id)
{
    //assert(!is_in_nursery(thread_descriptor, id));
    assert(id->h_tid & GCFLAG_OLD);

    size_t size = stmgc_size(obj);
    memcpy(id, obj, size);
    id->h_tid &= ~GCFLAG_HAS_ID;
    id->h_tid |= GCFLAG_OLD;
    dprintf(("copy_to_old_id_copy(%p -> %p)\n", obj, id));
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
    return n ^ (((urevision_t)n) >> 4);
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
    return mangle_hash(stm_id(p));
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
            return (revision_t)p;
        }

        dprintf(("stm_id(%p) has orig fst: %p\n", 
                 p, (gcptr)p->h_original));
        return p->h_original;
    } 
    else if (p->h_tid & GCFLAG_OLD) {
        /* old objects must have an h_original xOR be
           the original itself. */
        dprintf(("stm_id(%p) is old, orig=0 fst: %p\n", p, p));
        return (revision_t)p;
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
        dprintf(("stm_id(%p) has orig: %p\n", 
                 p, (gcptr)p->h_original));
    }
    else {
        /* must create shadow original object XXX: or use
           backup, if exists */
        
        /* XXX use stmgcpage_malloc() directly, we don't need to copy
         * the contents yet */
        gcptr O = stmgc_duplicate_old(p);
        p->h_original = (revision_t)O;
        p->h_tid |= GCFLAG_HAS_ID;
        
        if (p->h_tid & GCFLAG_PRIVATE_FROM_PROTECTED) {
            gcptr B = (gcptr)p->h_revision;
            B->h_original = (revision_t)O;
        }
        
        result = (revision_t)O;
        dprintf(("stm_id(%p) young, make shadow %p\n", p, O));
    }
    
    spinlock_release(d->public_descriptor->collection_lock);
    return result;
}

_Bool stm_pointer_equal(gcptr p1, gcptr p2)
{
    /* fast path for two equal pointers */
    if (p1 == p2)
        return 1;
    /* types must be the same */
    if ((p1->h_tid & STM_USER_TID_MASK) != (p2->h_tid & STM_USER_TID_MASK))
        return 0;
    return stm_id(p1) == stm_id(p2);
}
