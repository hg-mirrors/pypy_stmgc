#define _GNU_SOURCE
#include "stmgc.h"
#include "stm/atomic.h"
#include "stm/list.h"
#include "stm/core.h"
#include "stm/pages.h"
#include "stm/gcpage.h"
#include "stm/sync.h"
#include "stm/largemalloc.h"

#include "stm/misc.c"
#include "stm/list.c"
#include "stm/pages.c"
#include "stm/prebuilt.c"
#include "stm/gcpage.c"
#include "stm/largemalloc.c"
#include "stm/nursery.c"
#include "stm/sync.c"
#include "stm/setup.c"
#include "stm/core.c"
