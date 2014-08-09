import os

def run_test(opt):
    err = os.system("clang -g -O%d -Werror -DRJBUF_CUSTOM_MALLOC -I../stm"
                    " -o test_rewind_O%d test_rewind.c ../stm/rewind_setjmp.c"
                    % (opt, opt))
    if err != 0:
        raise OSError("clang failed on test_rewind.c")
    for testnum in [1, 2, 3, 4, 5]:
        print '=== O%d: RUNNING TEST %d ===' % (opt, testnum)
        err = os.system("./test_rewind_O%d %d" % (opt, testnum))
        if err != 0:
            raise OSError("'test_rewind_O%d %d' failed" % (opt, testnum))
    os.unlink("./test_rewind_O%d" % (opt,))

def test_O0(): run_test(0)
def test_O1(): run_test(1)
def test_O2(): run_test(2)
def test_O3(): run_test(3)
