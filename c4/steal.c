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

void stm_steal_stub(gcptr P)
{
    struct tx_public_descriptor *foreign_pd = STUB_THREAD(P);

    spinlock_acquire(foreign_pd->collection_lock, 'S');   /*stealing*/

    revision_t v = ACCESS_ONCE(P->h_revision);
    if ((v & 3) != 2)
        goto done;     /* un-stubbed while we waited for the lock */

    gcptr L = (gcptr)(v - 2);

    if (L->h_tid & GCFLAG_PRIVATE_FROM_PROTECTED) {
        gcptr B = (gcptr)L->h_revision;     /* the backup copy */
        B->h_tid &= ~GCFLAG_BACKUP_COPY;
        L->h_revision = *foreign_pd->descriptor->private_revision_ref;

        /* add {B: L} in 'public_to_private', but lazily, because we don't
           want to walk over the feet of the foreign thread */
        B->h_tid |= GCFLAG_PUBLIC_TO_PRIVATE;
        gcptrlist_insert2(&foreign_pd->stolen_objects, B, L);

        L = B;
    }

    /* change L from protected to public */
    L->h_tid |= GCFLAG_PUBLIC;

    smp_wmb();      /* the following update must occur "after" the flag
                       GCFLAG_PUBLIC was added, for other threads */

    /* update the original P->h_revision to point directly to L */
    P->h_revision = (revision_t)L;

 done:
    spinlock_release(foreign_pd->collection_lock);
}

void stm_normalize_stolen_objects(struct tx_descriptor *d)
{
    spinlock_acquire(d->public_descriptor->collection_lock, 'N');

    long i, size = d->public_descriptor->stolen_objects.size;
    gcptr *items = d->public_descriptor->stolen_objects.items;

    for (i = 0; i < size; i += 2) {
        gcptr B = items[i];
        gcptr L = items[i + 1];
        g2l_insert(&d->public_to_private, B, L);
    }
    gcptrlist_clear(&d->public_descriptor->stolen_objects);

    spinlock_release(d->public_descriptor->collection_lock);
}
