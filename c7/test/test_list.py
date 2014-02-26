import random
import cffi
from common import parent_dir


ffi = cffi.FFI()
ffi.cdef("""
struct list_s *list_create(void);

struct tree_s *tree_create(void);
void tree_free(struct tree_s *tree);
void tree_clear(struct tree_s *tree);
bool tree_is_cleared(struct tree_s *tree);
bool tree_contains(struct tree_s *tree, uintptr_t addr);
void tree_insert(struct tree_s *tree, uintptr_t addr, uintptr_t val);
bool tree_delete_item(struct tree_s *tree, uintptr_t addr);
int test_tree_walk(struct tree_s *tree, uintptr_t addrs[]);
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

int test_tree_walk(struct tree_s *tree, uintptr_t addrs[])
{
    int result = 0;
    wlog_t *item;
    TREE_LOOP_FORWARD(*tree, item) {
        addrs[result++] = item->addr;
    } TREE_LOOP_END;
    int i = result;
    TREE_LOOP_BACKWARD(*tree, item) {
        assert(i > 0);
        i--;
        assert(addrs[i] == item->addr);
    } TREE_LOOP_END;
    assert(i == 0);
    return result;
}
''', define_macros=[('STM_TESTS', '1')],
     undef_macros=['NDEBUG'],
     include_dirs=[parent_dir],
     extra_compile_args=['-g', '-O0', '-Werror', '-ferror-limit=1'],
     force_generic_engine=True)

# ____________________________________________________________

# XXX need tests for list_xxx too

def test_tree_empty():
    t = lib.tree_create()
    for i in range(100):
        assert lib.tree_contains(t, i) == False
    lib.tree_free(t)

def test_tree_add():
    t = lib.tree_create()
    lib.tree_insert(t, 23000, 456)
    for i in range(0, 100000, 1000):
        assert lib.tree_contains(t, i) == (i == 23000)
    lib.tree_free(t)

def test_tree_is_cleared():
    t = lib.tree_create()
    assert lib.tree_is_cleared(t)
    lib.tree_insert(t, 23000, 456)
    assert not lib.tree_is_cleared(t)
    lib.tree_free(t)

def test_tree_delete_item():
    t = lib.tree_create()
    lib.tree_insert(t, 23000, 456)
    lib.tree_insert(t, 42000, 34289)
    assert not lib.tree_is_cleared(t)
    assert lib.tree_contains(t, 23000)
    res = lib.tree_delete_item(t, 23000)
    assert res
    assert not lib.tree_contains(t, 23000)
    res = lib.tree_delete_item(t, 23000)
    assert not res
    res = lib.tree_delete_item(t, 21000)
    assert not res
    assert not lib.tree_is_cleared(t)
    assert lib.tree_contains(t, 42000)
    res = lib.tree_delete_item(t, 42000)
    assert res
    assert not lib.tree_is_cleared(t)   # not cleared, but still empty
    for i in range(100):
        assert not lib.tree_contains(t, i)
    lib.tree_free(t)

def test_tree_walk():
    t = lib.tree_create()
    lib.tree_insert(t, 23000, 456)
    lib.tree_insert(t, 42000, 34289)
    a = ffi.new("uintptr_t[10]")
    res = lib.test_tree_walk(t, a)
    assert res == 2
    assert ((a[0] == 23000 and a[1] == 42000) or
            (a[0] == 42000 and a[1] == 23000))
    lib.tree_free(t)

def test_tree_walk_big():
    t = lib.tree_create()
    values = random.sample(xrange(0, 1000000, 8), 300)
    for x in values:
        lib.tree_insert(t, x, x)
    a = ffi.new("uintptr_t[1000]")
    res = lib.test_tree_walk(t, a)
    assert res == 300
    found = set()
    for i in range(res):
        found.add(a[i])
    assert found == set(values)
    lib.tree_free(t)
