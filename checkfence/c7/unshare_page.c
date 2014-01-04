#include "lsl_protos.h"


int __sync_lock_test_and_set(int *lock, int nvalue)
{
    /* the x86 behavior of this instruction, which is really a XCHG */
    int old = *lock;
    lsl_assume(lsl_cas_32(lock, old, nvalue));
    lsl_fence("load-load");
    return old;
}


enum { SHARED_PAGE=0, REMAPPING_PAGE, PRIVATE_PAGE };  /* flag_page_private */
int flag_page_private;
int privatized_data;

void INIT(void)
{
    flag_page_private = SHARED_PAGE;
}

void _stm_privatize(void)
{
    int previous = __sync_lock_test_and_set(&flag_page_private,
                                            REMAPPING_PAGE);
    switch (previous) {
    case PRIVATE_PAGE:
        lsl_assert(flag_page_private != SHARED_PAGE);
        flag_page_private = PRIVATE_PAGE;
        return;

    case REMAPPING_PAGE:
        lsl_assert(flag_page_private != SHARED_PAGE);
        /* here we wait until 'flag_page_private' is changed away from
           REMAPPING_PAGE, and we assume that it eventually occurs */
        lsl_assume(flag_page_private != REMAPPING_PAGE);
        lsl_fence("load-load");
        return;

    case SHARED_PAGE:
        lsl_observe_label("privatizing");
        privatized_data = 42;
        lsl_fence("store-store");
        lsl_assert(flag_page_private == REMAPPING_PAGE);
        flag_page_private = PRIVATE_PAGE;
        return;
    }
    lsl_assert(0);
}

void PRIVATIZE(void)
{
    _stm_privatize();
    int data = privatized_data;
    lsl_observe_output("data", data);
}
