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


# note that 'build' is partially optimized but still contains all asserts
debug-%: %.c ${H_FILES} ${C_FILES}
	clang -I.. -pthread -DSTM_DEBUGPRINT -DSTM_GC_NURSERY=128 -g -O0 \
        $< -o debug-$* -Wall -Werror ../stmgc.c

build-%: %.c ${H_FILES} ${C_FILES}
	clang -I.. -pthread -DSTM_GC_NURSERY=128 -g -O1 \
        $< -o build-$* -Wall -Werror ../stmgc.c

release-%: %.c ${H_FILES} ${C_FILES}
	clang -I.. -pthread  -g -DNDEBUG -O2 $< -o release-$* \
        -Wall -Werror ../stmgc.c
