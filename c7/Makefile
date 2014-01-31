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


H_FILES = core.h list.h pagecopy.h reader_writer_lock.h stmsync.h pages.h nursery.h largemalloc.h

C_FILES = core.c list.c pagecopy.c reader_writer_lock.c stmsync.c pages.c nursery.c largemalloc.c

DEBUG = -g 


# note that we don't say -DNDEBUG, so that asserts should still be compiled in
# also, all debug code with extra checks but not the debugprints
build-%: %.c ${H_FILES} ${C_FILES} 
	clang -pthread  -g -O1  $< -o build-$* -Wall ${C_FILES}
release-%: %.c ${H_FILES} ${C_FILES} 
	clang -pthread  -g -DNDEBUG -O2 $< -o release-$* -Wall ${C_FILES}
