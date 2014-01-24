#include <stdlib.h>
#include "core.h"

void stm_largemalloc_init(char *data_start, size_t data_size);
int stm_largemalloc_resize_arena(size_t new_size);

object_t *stm_large_malloc(size_t request_size);
void stm_large_free(object_t *data);

void _stm_large_dump(void);
char *_stm_largemalloc_data_start(void);

void _stm_move_object(object_t *obj, char *src, char *dst);
size_t _stm_data_size(struct object_s *data);
void _stm_chunk_pages(struct object_s *data, uintptr_t *start, uintptr_t *num);
                

