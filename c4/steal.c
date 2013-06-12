#include "stmimpl.h"


#define STUB_PAGE         (4096 - 2*WORD)
#define STUB_NB_OBJS      ((STUB_BLOCK_SIZE - 2*WORD) /    \
                             sizeof(struct stm_object_s))

struct stub_block_s {
    struct tx_public_descriptor *thread;
    struct stub_block_s *next;
    struct stm_object_s stubs[STUB_NB_OBJS];
};

gcptr stm_stub_malloc(struct tx_public_descriptor *pd)
{
    assert(pd->collection_lock != 0);

    gcptr p = pd->stub_free_list;
    if (p == NULL) {
        assert(sizeof(struct stub_block_s) == STUB_BLOCK_SIZE);

        char *page = stm_malloc(STUB_PAGE);
        char *page_end = page + STUB_PAGE;
        page += (-(revision_t)page) & (STUB_BLOCK_SIZE-1);  /* round up */

        struct stub_block_s *b = (struct stub_block_s *)page;
        struct stub_block_s *nextb = NULL;
        gcptr nextp = NULL;
        int i;

        while (((char *)(b + 1)) <= page_end) {
            b->thread = pd;
            b->next = nextb;
            for (i = 0; i < STUB_NB_OBJS; i++) {
                b->stubs[i].h_revision = (revision_t)nextp;
                nextp = &b->stubs[i];
            }
            b++;
        }
        assert(nextp != NULL);
        p = nextp;
    }
    pd->stub_free_list = (gcptr)p->h_revision;
    assert(STUB_THREAD(p) == pd);
    return p;
}


struct tx_steal_data {
    struct tx_public_descriptor *foreign_pd;
    struct G2L all_stubs;   /* { protected: public_stub } */
};
static __thread struct tx_steal_data *steal_data;

static void replace_ptr_to_protected_with_stub(gcptr *pobj)
{
    gcptr stub, obj = *pobj;
    if (obj == NULL || (obj->h_tid & GCFLAG_PUBLIC) != 0)
        return;

    /* we use 'all_stubs', a dictionary, in order to try to avoid
       duplicate stubs for the same object.  XXX maybe it would be
       better to use a fast approximative cache that stays around for
       several stealings. */
    struct tx_steal_data *sd = steal_data;
    wlog_t *item;
    G2L_FIND(sd->all_stubs, obj, item, goto not_found);

    /* found already */
    stub = item->val;
    assert(stub->h_revision == (((revision_t)obj) | 2));
    goto done;

 not_found:
    stub = stm_stub_malloc(sd->foreign_pd);
    stub->h_tid = obj->h_tid | GCFLAG_PUBLIC | GCFLAG_STUB;
    stub->h_revision = ((revision_t)obj) | 2;
    g2l_insert(&sd->all_stubs, obj, stub);

 done:
    *pobj = stub;
    fprintf(stderr, "  stolen: fixing *%p: %p -> %p\n", pobj, obj, stub);
}

void stm_steal_stub(gcptr P)
{
    struct tx_public_descriptor *foreign_pd = STUB_THREAD(P);

    spinlock_acquire(foreign_pd->collection_lock, 'S');   /*stealing*/

    revision_t v = ACCESS_ONCE(P->h_revision);
    if ((v & 3) != 2)
        goto done;     /* un-stubbed while we waited for the lock */

    gcptr L = (gcptr)(v - 2);

    /* L might be a private_from_protected, or just a protected copy.
       To know which case it is, read GCFLAG_PRIVATE_FROM_PROTECTED.
    */
    if (L->h_tid & GCFLAG_PRIVATE_FROM_PROTECTED) {
        gcptr B = (gcptr)L->h_revision;     /* the backup copy */

        /* B is now a backup copy, i.e. a protected object, and we own
           the foreign thread's collection_lock, so we can read/write the
           flags
        */
        assert(B->h_tid & GCFLAG_BACKUP_COPY);
        B->h_tid &= ~GCFLAG_BACKUP_COPY;

        if (B->h_tid & GCFLAG_PUBLIC_TO_PRIVATE) {
            /* already stolen */
        }
        else {
            B->h_tid |= GCFLAG_PUBLIC_TO_PRIVATE;
            /* add {B: L} in 'public_to_private', but lazily, because we
               don't want to walk over the feet of the foreign thread
            */
            gcptrlist_insert2(&foreign_pd->stolen_objects, B, L);
        }
        fprintf(stderr, "stolen: %p -> %p - - -> %p\n", P, B, L);
        L = B;
    }
    else {
        fprintf(stderr, "stolen: %p -> %p\n", P, L);
    }

    /* Here L is a protected (or backup) copy, and we own the foreign
       thread's collection_lock, so we can read/write the flags.  Change
       it from protected to public.
    */
    L->h_tid |= GCFLAG_PUBLIC;

    /* Note that all protected or backup copies have a h_revision that
       is odd.
    */
    assert(L->h_revision & 1);

    /* At this point, the object can only be seen by its owning foreign
       thread and by us.  No 3rd thread can see it as long as we own
       the foreign thread's collection_lock.  For the foreign thread,
       it might suddenly see the GCFLAG_PUBLIC being added to L
       (but it may not do any change to the flags itself, because
       it cannot grab its own collection_lock).  L->h_revision is an
       odd number that is also valid on a public up-to-date object.
    */

    /* Fix the content of the object: we need to change all pointers
       that reference protected copies into pointers that reference
       stub copies.
    */
    struct tx_steal_data sd;
    sd.foreign_pd = foreign_pd;
    memset(&sd.all_stubs, 0, sizeof(sd.all_stubs));
    steal_data = &sd;
    stmcb_trace(L, &replace_ptr_to_protected_with_stub);
    g2l_delete_not_used_any_more(&sd.all_stubs);

    /* If another thread (the foreign or a 3rd party) does a read
       barrier from P, it must only reach L if all writes to L are
       visible; i.e. it must not see P->h_revision => L that still
       doesn't have the GCFLAG_PUBLIC.  So we need a CPU write
       barrier here.
    */
    smp_wmb();

    /* update the original P->h_revision to point directly to L */
    P->h_revision = (revision_t)L;

 done:
    spinlock_release(foreign_pd->collection_lock);
}

void stm_normalize_stolen_objects(struct tx_descriptor *d)
{
    long i, size = d->public_descriptor->stolen_objects.size;
    gcptr *items = d->public_descriptor->stolen_objects.items;

    for (i = 0; i < size; i += 2) {
        gcptr B = items[i];
        gcptr L = items[i + 1];

        assert(L->h_tid & GCFLAG_PRIVATE_FROM_PROTECTED);
        assert(!(B->h_tid & GCFLAG_BACKUP_COPY));  /* already removed */

        g2l_insert(&d->public_to_private, B, L);

        /* to be on the safe side */
        fxcache_remove(&d->recent_reads_cache, B);

        /* but this is definitely needed: all keys in public_to_private
           must appear in list_of_read_objects */
        fprintf(stderr, "n.readobj: %p\n", B);
        gcptrlist_insert(&d->list_of_read_objects, B);
    }
    gcptrlist_clear(&d->public_descriptor->stolen_objects);
}

gcptr _stm_find_stolen_objects(struct tx_descriptor *d, gcptr obj)
{
    /* read-only, for debugging */
    long i, size = d->public_descriptor->stolen_objects.size;
    gcptr *items = d->public_descriptor->stolen_objects.items;

    for (i = 0; i < size; i += 2) {
        gcptr B = items[i];
        gcptr L = items[i + 1];

        assert(L->h_tid & GCFLAG_PRIVATE_FROM_PROTECTED);
        if (B == obj)
            return L;
    }
    return NULL;
}
