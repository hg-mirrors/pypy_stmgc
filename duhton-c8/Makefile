
C8SOURCES = ../c8/stmgc.c ../c8/stm/*.c

C8HEADERS = ../c8/stmgc.h ../c8/stm/*.h

COMMON = -pthread -lrt -g -Wall


all: duhton_debug duhton duhton_release

duhton: *.c *.h $(C8SOURCES) $(C8HEADERS)
	clang $(COMMON) -O2 -o duhton *.c ../c8/stmgc.c

duhton_release: *.c *.h $(C8SOURCES) $(C8HEADERS)
	clang $(COMMON) -DNDEBUG -O2 -o duhton_release *.c ../c8/stmgc.c


duhton_debug: *.c *.h $(C8SOURCES) $(C8HEADERS)
	clang -DSTM_DEBUGPRINT $(COMMON) -DDu_DEBUG -o duhton_debug *.c ../c8/stmgc.c


clean:
	rm -f duhton duhton_debug duhton_release
