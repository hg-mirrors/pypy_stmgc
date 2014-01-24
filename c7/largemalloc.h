#include <stdlib.h>
#include "core.h"

void stm_largemalloc_init(char *data_start, size_t data_size);
int stm_largemalloc_resize_arena(size_t new_size);

object_t *stm_large_malloc(size_t request_size);
void stm_large_free(object_t *data);

void _stm_large_dump(void);
void _stm_chunk_pages(object_t *tldata, intptr_t *start, intptr_t *num);
size_t _stm_data_size(object_t *tldata);
char *_stm_largemalloc_data_start(void);
