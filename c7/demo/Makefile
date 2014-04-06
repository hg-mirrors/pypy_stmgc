#
# Makefile for the demos.
#

DEBUG_EXE = debug-demo2
BUILD_EXE = build-demo2
RELEASE_EXE = release-demo2

debug: $(DEBUG_EXE)       # with prints and asserts
build: $(BUILD_EXE)       # without prints, but with asserts
release: $(RELEASE_EXE)   # without prints nor asserts

clean:
	rm -f $(BUILD_EXE) $(DEBUG_EXE) $(RELEASE_EXE)


H_FILES = ../stmgc.h ../stm/*.h
C_FILES = ../stmgc.c ../stm/*.c

COMMON = -I.. -pthread -lrt -g -Wall -Werror -DSTM_LARGEMALLOC_TEST


# note that 'build' is partially optimized but still contains all asserts
debug-%: %.c ${H_FILES} ${C_FILES}
	clang $(COMMON) -DSTM_DEBUGPRINT -DSTM_GC_NURSERY=128 -O0 \
        $< -o debug-$* ../stmgc.c

build-%: %.c ${H_FILES} ${C_FILES}
	clang $(COMMON) -DSTM_GC_NURSERY=128 -O1 $< -o build-$* ../stmgc.c

release-%: %.c ${H_FILES} ${C_FILES}
	clang $(COMMON) -DNDEBUG -O2 $< -o release-$* ../stmgc.c


release-htm-%: %.c ../../htm-c7/stmgc.? ../../htm-c7/htm.h
	clang $(COMMON) -O2 $< -o release-htm-$* ../../htm-c7/stmgc.c -DUSE_HTM
