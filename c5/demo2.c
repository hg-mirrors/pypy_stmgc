#include <stdlib.h>
#include <stdio.h>


char *stm_large_malloc(size_t request_size);


int main()
{
    printf("%p\n", stm_large_malloc(10000));
    printf("%p\n", stm_large_malloc(10000));
    printf("%p\n", stm_large_malloc(10000));
    return 0;
}
