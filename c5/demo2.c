#include <stdlib.h>
#include <stdio.h>
#include <assert.h>


#include "largemalloc.h"

#define dump() _stm_large_dump()


char buffer[65536];


int main()
{
    stm_largemalloc_init(buffer, sizeof(buffer));

    char *d1 = stm_large_malloc(7000);
    char *d2 = stm_large_malloc(8000);
    /*char *d3 = */ stm_large_malloc(9000);

    dump();

    stm_large_free(d1);
    stm_large_free(d2);

    dump();

    char *d4 = stm_large_malloc(600);
    assert(d4 == d1);
    char *d5 = stm_large_malloc(600);
    assert(d5 == d4 + 616);

    dump();

    stm_large_free(d5);

    dump();

    stm_large_malloc(600);
    stm_large_free(d4);

    dump();

    stm_large_malloc(608);

    dump();

    return 0;
}
