#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <time.h>

#include "stmgc.h"
#include "../stm/largemalloc.h"

static inline double get_stm_time(void)
{
    struct timespec tp;
    clock_gettime(CLOCK_MONOTONIC, &tp);
    return tp.tv_sec + tp.tv_nsec * 0.000000001;
}

ssize_t stmcb_size_rounded_up(struct object_s *ob)
{
    abort();
}

void stmcb_trace(struct object_s *obj, void visit(object_t **))
{
    abort();
}

void stmcb_commit_soon() {}

/************************************************************/

#define ARENA_SIZE  (1024*1024*1024)

static char *arena_data;
extern bool (*_stm_largemalloc_keep)(char *data);   /* a hook for tests */
void _stm_mutex_pages_lock(void);


static bool keep_me(char *data) {
    static bool last_answer = false;
    last_answer = !last_answer;
    return last_answer;
}

void timing(int scale)
{
    long limit = 1L << scale;
    _stm_largemalloc_init_arena(arena_data, ARENA_SIZE);
    double start = get_stm_time();

    long i;
    for (i = 0; i < limit; i++) {
        _stm_large_malloc(16 + 8 * (i % 4));  /* may return NULL */
    }
    _stm_largemalloc_keep = keep_me;
    _stm_largemalloc_sweep();
    for (i = 0; i < limit; i++) {
        _stm_large_malloc(16 + 8 * (i % 4));  /* may return NULL */
    }

    double stop = get_stm_time();
    printf("scale %2d: %.9f\n", scale, stop - start);
}



int main(void)
{
    int i;
    arena_data = malloc(ARENA_SIZE);
    assert(arena_data != NULL);
    _stm_mutex_pages_lock();
    for (i = 0; i < 25; i++)
        timing(i);
    return 0;
}
