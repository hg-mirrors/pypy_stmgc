#include "stmimpl.h"


/* For statistics */
static uintptr_t count_global_pages;

/* For tests */
long stmgcpage_count(int quantity)
{
    switch (quantity) {
    case 0: return count_global_pages;
    case 1: return LOCAL_GCPAGES()->count_pages;
    default: return -1;
    }
}
