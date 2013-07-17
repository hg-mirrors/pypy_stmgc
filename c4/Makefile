#
# Makefile for the demos.
#

DEBUG_EXE = debug-demo1 debug-demo2 debug-demo_random
BUILD_EXE = build-demo1 build-demo2 build-demo_random
RELEASE_EXE = release-demo1 release-demo2 release-demo_random

debug: $(DEBUG_EXE)       # with prints and asserts
build: $(BUILD_EXE)       # without prints, but with asserts
release: $(RELEASE_EXE)   # without prints nor asserts

clean:
	rm -f $(BUILD_EXE) $(DEBUG_EXE) $(RELEASE_EXE)


H_FILES = atomic_ops.h stmgc.h stmimpl.h \
	  et.h lists.h steal.h nursery.h gcpage.h \
          stmsync.h extra.h weakref.h dbgmem.h fprintcolor.h

C_FILES = et.c lists.c steal.c nursery.c gcpage.c \
          stmsync.c extra.c weakref.c dbgmem.c fprintcolor.c

DEBUG = -g -DGC_NURSERY=0x10000 -D_GC_DEBUG=1 -DDUMP_EXTRA=1 -D_GC_DEBUGPRINTS=1


# note that we don't say -DNDEBUG, so that asserts should still be compiled in
# also, all debug code with extra checks but not the debugprints
build-%: %.c ${H_FILES} ${C_FILES} stmgc.c
	gcc -pthread -DGC_NURSERY=0x10000  -D_GC_DEBUG=1 -g $< -o build-$* -Wall stmgc.c -lrt

debug-%: %.c ${H_FILES} ${C_FILES}
	gcc -pthread -DDUMP_EXTRA=1 ${DEBUG} $< -o debug-$* -Wall ${C_FILES} -lrt

release-%: %.c ${H_FILES} ${C_FILES} stmgc.c
	gcc -pthread -DNDEBUG -O2 -g $< -o release-$* -Wall stmgc.c -lrt

test-%:
	./$* 2>/dev/null | grep "check ok"
