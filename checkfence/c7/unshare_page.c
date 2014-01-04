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

void _stm_privatize(int mode)
{
    int was_shared;
    if (mode == 0) {
        int previous = __sync_lock_test_and_set(&flag_page_private,
                                                REMAPPING_PAGE);
        if (previous == PRIVATE_PAGE) {
            lsl_assert(flag_page_private != SHARED_PAGE);
            flag_page_private = PRIVATE_PAGE;
            return;
        }
        was_shared = (previous == SHARED_PAGE);
    }
    else {
        was_shared = lsl_cas_32(&flag_page_private,
                                SHARED_PAGE, REMAPPING_PAGE);
        lsl_fence("load-load");
    }

    if (!was_shared) {
        lsl_assert(flag_page_private != SHARED_PAGE);
        /* here we wait until 'flag_page_private' is changed away from
           REMAPPING_PAGE, and we assume that it eventually occurs */
        lsl_assume(flag_page_private != REMAPPING_PAGE);
        lsl_fence("load-load");
    }
    else {
        lsl_observe_label("privatizing");
        privatized_data = 42;
        lsl_fence("store-store");
        lsl_assert(flag_page_private == REMAPPING_PAGE);
        flag_page_private = PRIVATE_PAGE;
    }
}

void PRIV_X86(void)
{
    _stm_privatize(0);
    int data = privatized_data;
    lsl_observe_output("data", data);
}

void PRIV_GEN(void)
{
    _stm_privatize(1);
    int data = privatized_data;
    lsl_observe_output("data", data);
}
