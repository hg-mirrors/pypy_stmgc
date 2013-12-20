#include <stdlib.h>
#include <stdio.h>


char *stm_large_malloc(size_t request_size);


static void dump(char *data)
{
    fprintf(stderr, "[ %p: %zu\n", data - 16, *(size_t*)(data - 16));
    fprintf(stderr, "  %p: %zu\n", data - 8, *(size_t*)(data - 8));
    size_t n = (*(size_t*)(data - 8)) & ~1;
    fprintf(stderr, "  %p: %zu ]\n", data + n, *(size_t*)(data + n));
}

int main()
{
    char *d1 = stm_large_malloc(10000);
    char *d2 = stm_large_malloc(10000);
    char *d3 = stm_large_malloc(10000);

    dump(d1);
    dump(d2);
    dump(d3);
    dump(d3 + 10016);

    return 0;
}
