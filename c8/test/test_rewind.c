#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "rewind_setjmp.h"


rewind_jmp_thread gthread;
int gevents[1000];
int num_gevents = 0;

void gevent(int num)
{
    assert(num_gevents <= sizeof(gevents) / sizeof(int));
    gevents[num_gevents++] = num;
}

void check_gevents(int expected[], int expected_size)
{
    int i;
    int expected_count = expected_size / sizeof(int);
    for (i = 0; i < expected_count && i < num_gevents; i++) {
        assert(gevents[i] == expected[i]);
    }
    assert(num_gevents == expected_count);
}

#define CHECK(expected)  check_gevents(expected, sizeof(expected))

/************************************************************/

__attribute__((noinline))
void f1(int x)
{
    gevent(1);
    if (x < 10) {
        rewind_jmp_longjmp(&gthread);
    }
}

static int test1_x;

void test1(void)
{
    rewind_jmp_buf buf;
    rewind_jmp_enterframe(&gthread, &buf, NULL);

    test1_x = 0;
    rewind_jmp_setjmp(&gthread, NULL);

    test1_x++;
    f1(test1_x);

    assert(test1_x == 10);
    int expected[] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
    CHECK(expected);

    assert(rewind_jmp_armed(&gthread));
    rewind_jmp_forget(&gthread);
    assert(!rewind_jmp_armed(&gthread));

    rewind_jmp_leaveframe(&gthread, &buf, NULL);
}

/************************************************************/

static int test2_x;

__attribute__((noinline))
int f2(void)
{
    rewind_jmp_buf buf;
    rewind_jmp_enterframe(&gthread, &buf, NULL);
    test2_x = 0;
    rewind_jmp_setjmp(&gthread, NULL);
    rewind_jmp_leaveframe(&gthread, &buf, NULL);
    return ++test2_x;
}

void test2(void)
{
    rewind_jmp_buf buf;
    rewind_jmp_enterframe(&gthread, &buf, NULL);
    int x = f2();
    gevent(x);
    if (x < 10)
        rewind_jmp_longjmp(&gthread);
    rewind_jmp_leaveframe(&gthread, &buf, NULL);
    int expected[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    CHECK(expected);
}

/************************************************************/

__attribute__((noinline))
int f3(int rec)
{
    if (rec > 0)
        return f3(rec - 1);
    else
        return f2();
}

void test3(void)
{
    rewind_jmp_buf buf;
    rewind_jmp_enterframe(&gthread, &buf, NULL);
    int x = f3(50);
    gevent(x);
    if (x < 10)
        rewind_jmp_longjmp(&gthread);
    rewind_jmp_leaveframe(&gthread, &buf, NULL);
    int expected[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    CHECK(expected);
}

/************************************************************/

__attribute__((noinline))
int f4(int rec)
{
    rewind_jmp_buf buf;
    rewind_jmp_enterframe(&gthread, &buf, NULL);
    int res;
    if (rec > 0)
        res = f4(rec - 1);
    else
        res = f2();
    rewind_jmp_leaveframe(&gthread, &buf, NULL);
    return res;
}

void test4(void)
{
    rewind_jmp_buf buf;
    rewind_jmp_enterframe(&gthread, &buf, NULL);
    int x = f4(5);
    gevent(x);
    if (x < 10)
        rewind_jmp_longjmp(&gthread);
    rewind_jmp_leaveframe(&gthread, &buf, NULL);
    int expected[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    CHECK(expected);
}

/************************************************************/

void test5(void)
{
    struct { int a; rewind_jmp_buf buf; int b; } sbuf;
    rewind_jmp_enterframe(&gthread, &sbuf.buf, NULL);
    sbuf.a = 42;
    sbuf.b = -42;
    test2_x = 0;
    rewind_jmp_setjmp(&gthread, NULL);
    sbuf.a++;
    sbuf.b--;
    gevent(sbuf.a);
    gevent(sbuf.b);
    if (test2_x == 0) {
        test2_x++;
        rewind_jmp_longjmp(&gthread);
    }
    int expected[] = {43, -43, 43, -43};
    CHECK(expected);
    rewind_jmp_leaveframe(&gthread, &sbuf.buf, NULL);
}

/************************************************************/

static int test6_x;

__attribute__((noinline))
void foo(int *x) { ++*x; }

__attribute__((noinline))
void f6(int c1, int c2, int c3, int c4, int c5, int c6, int c7,
        int c8, int c9, int c10, int c11, int c12, int c13)
{
    rewind_jmp_buf buf;
    rewind_jmp_enterframe(&gthread, &buf, NULL);

    int a1 = c1;
    int a2 = c2;
    int a3 = c3;
    int a4 = c4;
    int a5 = c5;
    int a6 = c6;
    int a7 = c7;
    int a8 = c8;
    int a9 = c9;
    int a10 = c10;
    int a11 = c11;
    int a12 = c12;
    int a13 = c13;

    rewind_jmp_setjmp(&gthread, NULL);
    gevent(a1); gevent(a2); gevent(a3); gevent(a4);
    gevent(a5); gevent(a6); gevent(a7); gevent(a8);
    gevent(a9); gevent(a10); gevent(a11); gevent(a12);
    gevent(a13);
    if (++test6_x < 4) {
        foo(&a1);
        foo(&a2);
        foo(&a3);
        foo(&a4);
        foo(&a5);
        foo(&a6);
        foo(&a7);
        foo(&a8);
        foo(&a9);
        foo(&a10);
        foo(&a11);
        foo(&a12);
        foo(&a13);
        rewind_jmp_longjmp(&gthread);
    }
    rewind_jmp_leaveframe(&gthread, &buf, NULL);
}

void test6(void)
{
    rewind_jmp_buf buf;
    rewind_jmp_enterframe(&gthread, &buf, NULL);
    test6_x = 0;
    f6(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13);
    rewind_jmp_leaveframe(&gthread, &buf, NULL);
    int expected[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13,
                      1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13,
                      1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13,
                      1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13};
    CHECK(expected);
}

/************************************************************/

static void *ssarray[99];

void testTL1(void)
{
    void *a4, *a5;
    rewind_jmp_buf buf;
    rewind_jmp_enterframe(&gthread, &buf, ssarray+5);

    a4 = (void *)444444;
    a5 = (void *)555555;
    ssarray[4] = a4;
    ssarray[5] = a5;

    if (rewind_jmp_setjmp(&gthread, ssarray+6) == 0) {
        /* first path */
        assert(ssarray[4] == a4);
        assert(ssarray[5] == a5);
        ssarray[4] = NULL;
        ssarray[5] = NULL;
        rewind_jmp_restore_shadowstack(&gthread);
        rewind_jmp_longjmp(&gthread);
    }
    /* second path */
    assert(ssarray[4] == NULL);   /* was not saved */
    assert(ssarray[5] == a5);     /* saved and restored */

    rewind_jmp_leaveframe(&gthread, &buf, ssarray+5);
}

__attribute__((noinline))
int gtl2(void)
{
    rewind_jmp_buf buf;
    rewind_jmp_enterframe(&gthread, &buf, ssarray+5);
    ssarray[5] = (void *)555555;

    int result = rewind_jmp_setjmp(&gthread, ssarray+6);

    assert(ssarray[4] == (void *)444444);
    assert(ssarray[5] == (void *)555555);
    ssarray[5] = NULL;

    rewind_jmp_leaveframe(&gthread, &buf, ssarray+5);
    return result;
}

void testTL2(void)
{
    rewind_jmp_buf buf;
    rewind_jmp_enterframe(&gthread, &buf, ssarray+4);

    ssarray[4] = (void *)444444;
    int result = gtl2();
    ssarray[4] = NULL;

    if (result == 0) {
        rewind_jmp_restore_shadowstack(&gthread);
        rewind_jmp_longjmp(&gthread);
    }

    rewind_jmp_leaveframe(&gthread, &buf, ssarray+4);
}

/************************************************************/

__attribute__((noinline))
int _7start_transaction()
{
    int result = rewind_jmp_setjmp(&gthread, NULL);
    return result;
}

__attribute__((noinline))
int _7enter_callback(rewind_jmp_buf *buf)
{
    rewind_jmp_enterprepframe(&gthread, buf, NULL);
    return _7start_transaction();
}

__attribute__((noinline))
int _7bootstrap()
{
    rewind_jmp_longjmp(&gthread);
    return 0;
}

__attribute__((noinline))
int _7leave_callback(rewind_jmp_buf *buf)
{
    rewind_jmp_leaveframe(&gthread, buf, NULL);
    return 0;
}

void test7(void)
{
    rewind_jmp_buf buf;
    register long bla = 3;
    rewind_jmp_prepareframe(&buf);
    if (_7enter_callback(&buf) == 0) {
        _7bootstrap();
    }
    _7leave_callback(&buf);
    assert(bla == 3);
}

/************************************************************/

int rj_malloc_count = 0;

void *rj_malloc(size_t size)
{
    rj_malloc_count++;
    void *ptr = malloc(size);
    fprintf(stderr, "malloc(%ld) -> %p\n", (long)size, ptr);
    return ptr;
}

void rj_free(void *ptr)
{
    if (ptr)
        rj_malloc_count--;
    fprintf(stderr, "free(%p)\n", ptr);
    free(ptr);
}


int main(int argc, char *argv[])
{
    assert(argc > 1);
    if (!strcmp(argv[1], "1"))       test1();
    else if (!strcmp(argv[1], "2"))  test2();
    else if (!strcmp(argv[1], "3"))  test3();
    else if (!strcmp(argv[1], "4"))  test4();
    else if (!strcmp(argv[1], "5"))  test5();
    else if (!strcmp(argv[1], "6"))  test6();
    else if (!strcmp(argv[1], "7"))  test7();
    else if (!strcmp(argv[1], "TL1")) testTL1();
    else if (!strcmp(argv[1], "TL2")) testTL2();
    else
        assert(!"bad argv[1]");
    assert(rj_malloc_count == 0);
    return 0;
}
