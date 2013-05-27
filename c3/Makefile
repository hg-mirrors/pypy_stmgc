#
# Run with "make -jN" for maximum randomness.
#

debug: debug-demo1 debug-demo2

clean:
	rm -f debug-demo1 debug-demo2


tests: iteration-1 iteration-2 iteration-3 iteration-4

all-builds: build-demo1 build-demo2 build-demo3 build-demo4 build-demo5

iteration-1: all-builds
	+make test-demo5 test-demo1 test-demo2 test-demo3 test-demo4

iteration-2: all-builds
	+make test-demo2 test-demo3 test-demo4 test-demo5 test-demo1

iteration-3: all-builds
	+make test-demo3 test-demo4 test-demo5 test-demo1 test-demo2

iteration-4: all-builds
	+make test-demo4 test-demo5 test-demo1 test-demo2 test-demo3


H_FILES = et.h lists.h nursery.h gcpage.h stmsync.h dbgmem.h fprintcolor.h stmgc.h atomic_ops.h stmimpl.h
C_FILES = et.c lists.c nursery.c gcpage.c stmsync.c dbgmem.c fprintcolor.c

DEBUG = -g -DGC_NURSERY=0x10000 -D_GC_DEBUG=1


build-%: %.c ${H_FILES} ${C_FILES}
	gcc -lrt -pthread -O2 -g $< -o $* -Wall ${C_FILES}

debug-%: %.c ${H_FILES} ${C_FILES}
	gcc -lrt -pthread ${DEBUG} $< -o debug-$* -Wall ${C_FILES}

test-%:
	./$* 2>/dev/null | grep "check ok"
