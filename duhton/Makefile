
C7SOURCES = ../c7/stmgc.c ../c7/stm/*.c

C7HEADERS = ../c7/stmgc.h ../c7/stm/*.h


all: duhton_debug duhton 

duhton: *.c *.h $(C7SOURCES) $(C7HEADERS)
	clang -pthread -g -O2 -o duhton *.c ../c7/stmgc.c -Wall

duhton_release: *.c *.h $(C7SOURCES) $(C7HEADERS)
	clang -pthread -g -DNDEBUG -O2 -o duhton_release *.c ../c7/stmgc.c -Wall


duhton_debug: *.c *.h $(C7SOURCES) $(C7HEADERS)
	clang -DSTM_DEBUGPRINT -pthread -g -DDu_DEBUG -o duhton_debug *.c ../c7/stmgc.c -Wall


duhton_nostm: *.c *.h ../gil-c7/stmgc.?
	clang -pthread -g -DNDEBUG -O2 -o duhton_nostm *.c ../gil-c7/stmgc.c -Wall  -DUSE_GIL


duhton_debug_nostm: *.c *.h ../gil-c7/stmgc.?
	clang -DSTM_DEBUGPRINT -pthread -g -DDu_DEBUG -o duhton_debug_nostm *.c ../gil-c7/stmgc.c -Wall -DUSE_GIL -ferror-limit=1


duhton_htm: *.c *.h ../htm-c7/stmgc.? ../htm-c7/htm.h
	clang -pthread -g -DNDEBUG -O2 -o duhton_htm *.c ../htm-c7/stmgc.c -Wall  -DUSE_HTM


duhton_debug_htm: *.c *.h ../htm-c7/stmgc.? ../htm-c7/htm.h
	clang -DSTM_DEBUGPRINT -pthread -g -DDu_DEBUG -o duhton_debug_htm *.c ../htm-c7/stmgc.c -Wall -DUSE_HTM -ferror-limit=1


clean:
	rm -f duhton duhton_debug duhton_release
