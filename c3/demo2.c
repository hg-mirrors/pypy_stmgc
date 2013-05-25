#include "stmgc.h"

/* rename these functions to always call their old-object version */
#define stm_allocate_object_of_size  _stm_allocate_object_of_size_old
#define stm_allocate                 _stm_allocate_old

#include "demo1.c"
