import cffi
from common import parent_dir


ffi = cffi.FFI()
ffi.cdef("""
struct list_s *list_create(void);

struct tree_s *tree_create(void);
void tree_free(struct tree_s *tree);
void tree_clear(struct tree_s *tree);
bool tree_contains(struct tree_s *tree, uintptr_t addr);
void tree_insert(struct tree_s *tree, uintptr_t addr, uintptr_t val);
bool tree_delete_item(struct tree_s *tree, uintptr_t addr);
""")

lib = ffi.verify('''
#include <stdbool.h>
#include <string.h>
#include <assert.h>

#define LIKELY(x)    (x)
#define UNLIKELY(x)  (x)
#define stm_fatalerror(x)  abort()

#include "stm/list.h"

#define _STM_CORE_H_
#include "stm/list.c"
''', define_macros=[('STM_TESTS', '1')],
     undef_macros=['NDEBUG'],
     include_dirs=[parent_dir],
     extra_compile_args=['-g', '-O0', '-Werror', '-ferror-limit=1'],
     force_generic_engine=True)

# ____________________________________________________________

def test_tree_empty():
    t = lib.tree_create()
    for i in range(100):
        assert lib.tree_contains(t, i) == False
    lib.tree_free(t)

def test_tree_add():
    t = lib.tree_create()
    lib.tree_insert(t, 23, 456)
    for i in range(100):
        assert lib.tree_contains(t, i) == (i == 23)
    lib.tree_free(t)
