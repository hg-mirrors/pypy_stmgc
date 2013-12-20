#include <stdlib.h>

void stm_largemalloc_init(char *data_start, size_t data_size);
int stm_largemalloc_resize_arena(size_t new_size);

char *stm_large_malloc(size_t request_size);
void stm_large_free(char *data);

void _stm_large_dump(void);
