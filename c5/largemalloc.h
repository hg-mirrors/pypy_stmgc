#include <stdlib.h>

char *stm_large_malloc(size_t request_size);
void stm_large_free(char *data);
void _stm_large_dump(char *data);
void _stm_large_reset(void);
