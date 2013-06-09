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
    revision_t w = L->h_revision;

    if (w == *foreign_pd->private_revision_ref) {
        /* The stub points to a private object L.  Because it cannot point
           to "really private" objects, it must mean that L used to be
           a protected object, and it has an attached backed copy.
           XXX find a way to optimize this search, maybe */
        long i, size = foreign_pd->active_backup_copies.size;
        gcptr *items = foreign_pd->active_backup_copies.items;
        for (i = size - 2; ; i -= 2) {
            assert(i >= 0);
            if (items[i] == L)
                break;
        }
        L = items[i + 1];
        assert(L->h_tid & GCFLAG_BACKUP_COPY);
        L->h_tid &= ~GCFLAG_BACKUP_COPY;
    }
    else if (L->h_tid & GCFLAG_PUBLIC) {
        /* The stub already points to a public object */
        goto unstub;
    }
    else if (!(w & 1)) {
        /* The stub points to a protected object L which has a backup
           copy attached.  Forget the backup copy. */
        w = ((gcptr)w)->h_revision;
        assert(w & 1);
        L->h_revision = w;
    }
    /* turn L into a public object */
    L->h_tid |= GCFLAG_PUBLIC;

 unstub:
    P->h_revision = (revision_t)L;

 done:
    spinlock_release(foreign_pd->collection_lock);
}

void stm_normalize_stolen_objects(struct tx_public_descriptor *pd)
{
    long i, size = pd->stolen_objects.size;
    gcptr *items = pd->stolen_objects.items;
    for (i = 0; i < size; i += 2) {
        gcptr L = items[i];
        gcptr Q = items[i + 1];
        if (L->h_revision == stm_private_rev_num) {
            
        }
    }
}
