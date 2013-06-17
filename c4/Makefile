#
# Run with "make -jN" for maximum randomness.
#

debug: debug-demo1

clean:
	rm -f debug-demo1


H_FILES = atomic_ops.h stmgc.h stmimpl.h \
	  et.h lists.h steal.h nursery.h gcpage.h \
          stmsync.h dbgmem.h fprintcolor.h

C_FILES = et.c lists.c steal.c nursery.c gcpage.c \
          stmsync.c dbgmem.c fprintcolor.c

DEBUG = -g -DGC_NURSERY=0x10000  #-D_GC_DEBUG=1


build-%: %.c ${H_FILES} ${C_FILES}
	gcc -lrt -pthread -O2 -g $< -o $* -Wall ${C_FILES}

debug-%: %.c ${H_FILES} ${C_FILES}
	gcc -lrt -pthread ${DEBUG} $< -o debug-$* -Wall ${C_FILES}

test-%:
	./$* 2>/dev/null | grep "check ok"
