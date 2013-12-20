#include <stdlib.h>
#include <stdio.h>
#include <assert.h>


char *stm_large_malloc(size_t request_size);
void stm_large_free(char *data);
void _stm_large_dump(char *data);

#define dump _stm_large_dump


int main()
{
    char *d1 = stm_large_malloc(7000);
    char *start = d1;
    char *d2 = stm_large_malloc(8000);
    char *d3 = stm_large_malloc(9000);

    dump(start);

    stm_large_free(d1);
    stm_large_free(d2);

    dump(start);

    char *d4 = stm_large_malloc(600);
    assert(d4 == d1);
    char *d5 = stm_large_malloc(600);
    assert(d5 == d4 + 616);

    dump(start);

    stm_large_free(d5);

    dump(start);

    stm_large_malloc(600);
    stm_large_free(d4);

    dump(start);

    stm_large_malloc(608);

    dump(start);

    return 0;
}
