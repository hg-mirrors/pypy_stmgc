#include <stdlib.h>
#include <stdio.h>
#include <assert.h>


char *stm_large_malloc(size_t request_size);
void stm_large_free(char *data);


static void dump(char *start)
{
    char *data = start;
    char *stop = start + 999999;

    while (data < stop) {
        fprintf(stderr, "[ %p: %zu\n", data - 16, *(size_t*)(data - 16));
        fprintf(stderr, "  %p: %zu ]\n", data - 8, *(size_t*)(data - 8));
        data += (*(size_t*)(data - 8)) & ~1;
        data += 16;
    }
    fprintf(stderr, ". %p: %zu\n\n", data - 16, *(size_t*)(data - 16));
}

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

    return 0;
}
