#
# Run with "make -jN" for maximum randomness.
#

debug: debug-demo1

clean:
	rm -f debug-demo1


H_FILES = atomic_ops.h stmgc.h stmimpl.h \
          dbgmem.h et.h fprintcolor.h lists.h steal.h stmsync.h
C_FILES = dbgmem.c et.c fprintcolor.c lists.c steal.c stmsync.c

DEBUG = -g -DGC_NURSERY=0x10000  #-D_GC_DEBUG=1


build-%: %.c ${H_FILES} ${C_FILES}
	gcc -lrt -pthread -O2 -g $< -o $* -Wall ${C_FILES}

debug-%: %.c ${H_FILES} ${C_FILES}
	gcc -lrt -pthread ${DEBUG} $< -o debug-$* -Wall ${C_FILES}

test-%:
	./$* 2>/dev/null | grep "check ok"
