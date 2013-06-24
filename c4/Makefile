#
# Makefile for the demos.
#

BUILD_EXE = build-demo1 build-demo2 build-demo_random
DEBUG_EXE = debug-demo1 debug-demo2 debug-demo_random

build: $(BUILD_EXE)
debug: $(DEBUG_EXE)

clean:
	rm -f $(BUILD_EXE) $(DEBUG_EXE)


H_FILES = atomic_ops.h stmgc.h stmimpl.h \
	  et.h lists.h steal.h nursery.h gcpage.h \
          stmsync.h dbgmem.h fprintcolor.h

C_FILES = et.c lists.c steal.c nursery.c gcpage.c \
          stmsync.c dbgmem.c fprintcolor.c

DEBUG = -g -DGC_NURSERY=0x10000 -D_GC_DEBUG=1


# note that we don't say -DNDEBUG, so that asserts should still be compiled it
build-%: %.c ${H_FILES} ${C_FILES} stmgc.c
	gcc -lrt -pthread -O2 -g $< -o build-$* -Wall stmgc.c

debug-%: %.c ${H_FILES} ${C_FILES}
	gcc -lrt -pthread ${DEBUG} $< -o debug-$* -Wall ${C_FILES}

test-%:
	./$* 2>/dev/null | grep "check ok"
