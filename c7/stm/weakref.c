#ifndef _STM_CORE_H_
# error "must be compiled via stmgc.c"
#endif


object_t *stm_allocate_weakref(ssize_t size_rounded_up)
{
    OPT_ASSERT(size_rounded_up > sizeof(struct object_s));
    object_t *obj = stm_allocate(size_rounded_up);
    obj->stm_flags |= GCFLAG_WEAKREF;
    LIST_APPEND(STM_PSEGMENT->young_weakrefs, obj);
    return obj;
}


/***** Minor collection *****/

void stm_move_young_weakrefs()
{
    /* The code relies on the fact that no weakref can be an old object
       weakly pointing to a young object.  Indeed, weakrefs are immutable
       so they cannot point to an object that was created after it.
    */
    LIST_FOREACH_R(
        STM_PSEGMENT->young_weakrefs,
        object_t * /*item*/,
        ({
            if (_is_in_nursery(item)) {
                object_t *TLPREFIX *pforwarded_array = (object_t *TLPREFIX *)item;

                /* the following checks are done like in nursery.c: */
                if (!(item->stm_flags & GCFLAG_HAS_SHADOW)
                    || (pforwarded_array[0] != GCWORD_MOVED)) {
                    /* weakref dies */
                    continue;
                }

                item = pforwarded_array[1]; /* moved location */
            }
            else {
                /* young outside nursery object */
                if (tree_contains(STM_PSEGMENT->young_outside_nursery,
                                  (uintptr_t)item)) {
                    /* still in the tree -> wasn't seen by the minor collection,
                       so it doesn't survive */
                    continue;
                }
            }
            assert(!_is_young(item));

            char *realobj = REAL_ADDRESS(STM_SEGMENT->segment_base, item);
            ssize_t size = stmcb_size_rounded_up((struct object_s *)realobj);
            object_t *pointing_to = *WEAKREF_PTR(item, size);
            assert(pointing_to != NULL);

            if (_is_in_nursery(pointing_to)) {
                object_t *TLPREFIX *pforwarded_array = (object_t *TLPREFIX *)pointing_to;
                /* the following checks are done like in nursery.c: */
                if (!(pointing_to->stm_flags & GCFLAG_HAS_SHADOW)
                    || (pforwarded_array[0] != GCWORD_MOVED)) {
                    /* pointing_to dies */
                    *WEAKREF_PTR(item, size) = NULL;
                    continue;   /* no need to remember in old_weakrefs */
                }
                else {
                    /* moved location */
                    *WEAKREF_PTR(item, size) = pforwarded_array[1];
                }
            }
            else {
                /* young outside nursery object or old object */
                if (tree_contains(STM_PSEGMENT->young_outside_nursery,
                                  (uintptr_t)pointing_to)) {
                    /* still in the tree -> wasn't seen by the minor collection,
                       so it doesn't survive */
                    *WEAKREF_PTR(item, size) = NULL;
                    continue;   /* no need to remember in old_weakrefs */
                }
                /* pointing_to was already old */
            }
            LIST_APPEND(STM_PSEGMENT->old_weakrefs, item);
        }));
    list_clear(STM_PSEGMENT->young_weakrefs);
}


/***** Major collection *****/

/* static _Bool is_partially_visited(gcptr obj) */
/* { */
/*     /\* Based on gcpage.c:visit_public().  Check the code here if we change */
/*        visit_public().  Returns True or False depending on whether we find any */
/*        version of 'obj' to be MARKED or not. */
/*     *\/ */
/*     assert(IMPLIES(obj->h_tid & GCFLAG_VISITED, */
/*                    obj->h_tid & GCFLAG_MARKED)); */
/*     if (obj->h_tid & GCFLAG_MARKED) */
/*         return 1; */

/*     /\* if (!(obj->h_tid & GCFLAG_PUBLIC)) *\/ */
/*     /\*     return 0; *\/ */
/*     assert(!(obj->h_tid & GCFLAG_PREBUILT_ORIGINAL)); */
/*     if (obj->h_original != 0) { */
/*         gcptr original = (gcptr)obj->h_original; */
/*         assert(IMPLIES(original->h_tid & GCFLAG_VISITED, */
/*                        original->h_tid & GCFLAG_MARKED)); */
/*         if (original->h_tid & GCFLAG_MARKED) */
/*             return 1; */
/*     } */
/*     return 0; */
/* } */

/* static void update_old_weakrefs_list(struct tx_public_descriptor *gcp) */
/* { */
/*     long i, size = gcp->old_weakrefs.size; */
/*     gcptr *items = gcp->old_weakrefs.items; */

/*     for (i = 0; i < size; i++) { */
/*         gcptr weakref = items[i]; */

/*         /\* if a weakref moved, update its position in the list *\/ */
/*         if (weakref->h_tid & GCFLAG_MOVED) { */
/*             items[i] = (gcptr)weakref->h_original; */
/*         } */
/*     } */
/* } */

/* static void visit_old_weakrefs(struct tx_public_descriptor *gcp) */
/* { */
/*     /\* Note: it's possible that a weakref points to a public stub to a */
/*        protected object, and only the protected object was marked as */
/*        VISITED so far.  In this case, this function needs to mark the */
/*        public stub as VISITED too. */
/*     *\/ */
/*     long i, size = gcp->old_weakrefs.size; */
/*     gcptr *items = gcp->old_weakrefs.items; */

/*     for (i = 0; i < size; i++) { */
/*         gcptr weakref = items[i]; */

/*         if (!(weakref->h_tid & GCFLAG_VISITED)) { */
/*             /\* the weakref itself dies *\/ */
/*         } */
/*         else { */
/*             /\* the weakref belongs to our thread, therefore we should */
/*                always see the most current revision here: *\/ */
/*             assert(weakref->h_revision & 1); */

/*             size_t size = stmgc_size(weakref); */
/*             gcptr pointing_to = *WEAKREF_PTR(weakref, size); */
/*             assert(pointing_to != NULL); */
/*             if (is_partially_visited(pointing_to)) { */
/*                 pointing_to = stmgcpage_visit(pointing_to); */
/*                 dprintf(("mweakref ptr moved %p->%p\n", */
/*                          *WEAKREF_PTR(weakref, size), */
/*                          pointing_to)); */

/*                 assert(pointing_to->h_tid & GCFLAG_VISITED); */
/*                 *WEAKREF_PTR(weakref, size) = pointing_to; */
/*             } */
/*             else { */
/*                 /\* the weakref appears to be pointing to a dying object, */
/*                    but we don't know for sure now.  Clearing it is left */
/*                    to clean_old_weakrefs(). *\/ */
/*             } */
/*         } */
/*     } */
/* } */

/* static void clean_old_weakrefs(struct tx_public_descriptor *gcp) */
/* { */
/*     long i, size = gcp->old_weakrefs.size; */
/*     gcptr *items = gcp->old_weakrefs.items; */

/*     for (i = size - 1; i >= 0; i--) { */
/*         gcptr weakref = items[i]; */
/*         assert(weakref->h_revision & 1); */
/*         if (weakref->h_tid & GCFLAG_VISITED) { */
/*             size_t size = stmgc_size(weakref); */
/*             gcptr pointing_to = *WEAKREF_PTR(weakref, size); */
/*             if (pointing_to->h_tid & GCFLAG_VISITED) { */
/*                 continue;   /\* the target stays alive, the weakref remains *\/ */
/*             } */
/*             dprintf(("mweakref lost ptr %p\n", *WEAKREF_PTR(weakref, size))); */
/*             *WEAKREF_PTR(weakref, size) = NULL;  /\* the target dies *\/ */
/*         } */
/*         /\* remove this weakref from the list *\/ */
/*         items[i] = items[--gcp->old_weakrefs.size]; */
/*     } */
/*     gcptrlist_compress(&gcp->old_weakrefs); */
/* } */

/* static void for_each_public_descriptor( */
/*                                   void visit(struct tx_public_descriptor *)) { */
/*     struct tx_descriptor *d; */
/*     for (d = stm_tx_head; d; d = d->tx_next) */
/*         visit(d->public_descriptor); */

/*     struct tx_public_descriptor *gcp; */
/*     revision_t index = -1; */
/*     while ((gcp = stm_get_free_public_descriptor(&index)) != NULL) */
/*         visit(gcp); */
/* } */

/* void stm_update_old_weakrefs_lists(void) */
/* { */
/*     /\* go over old weakrefs lists and update the list with possibly */
/*        new pointers because of copy_over_original *\/ */
/*     for_each_public_descriptor(update_old_weakrefs_list); */
/* } */


/* void stm_visit_old_weakrefs(void) */
/* { */
/*     /\* Figure out which weakrefs survive, which possibly */
/*        adds more objects to 'objects_to_trace'. */
/*     *\/ */
/*     for_each_public_descriptor(visit_old_weakrefs); */
/* } */

/* void stm_clean_old_weakrefs(void) */
/* { */
/*     /\* Clean up the non-surviving weakrefs */
/*      *\/ */
/*     for_each_public_descriptor(clean_old_weakrefs); */
/* } */
