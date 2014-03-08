#define _GNU_SOURCE 1
#include "stmgc.h"
#include "stm/atomic.h"
#include "stm/list.h"
#include "stm/core.h"
#include "stm/pagecopy.h"
#include "stm/pages.h"
#include "stm/gcpage.h"
#include "stm/sync.h"
#include "stm/largemalloc.h"
#include "stm/nursery.h"
#include "stm/contention.h"
#include "stm/fprintcolor.h"

#include "stm/misc.c"
#include "stm/list.c"
#include "stm/pagecopy.c"
#include "stm/pages.c"
#include "stm/prebuilt.c"
#include "stm/gcpage.c"
#include "stm/largemalloc.c"
#include "stm/nursery.c"
#include "stm/sync.c"
#include "stm/setup.c"
#include "stm/hash_id.c"
#include "stm/core.c"
#include "stm/contention.c"
#include "stm/fprintcolor.c"
