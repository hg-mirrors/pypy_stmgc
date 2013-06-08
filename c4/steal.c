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
    abort();
  struct tx_public_descriptor *foreign_pd;
  revision_t target_descriptor_index;
  revision_t v = ACCESS_ONCE(P->h_revision);
  if ((v & 3) != 2)
    return;
  target_descriptor_index = *(revision_t *)(v & ~(STUB_BLOCK_SIZE-1));
  foreign_pd = stm_descriptor_array[target_descriptor_index];
  abort();
}
