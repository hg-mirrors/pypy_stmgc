
C7SOURCES = ../c7/stmgc.c ../c7/stm/*.c

C7HEADERS = ../c7/stmgc.h ../c7/stm/*.h


all: duhton_debug duhton 

duhton: *.c *.h $(C7SOURCES) $(C7HEADERS)
	clang -pthread -g -O2 -o duhton *.c ../c7/stmgc.c -Wall

duhton_release: *.c *.h $(C7SOURCES) $(C7HEADERS)
	clang -pthread -g -DNDEBUG -O2 -o duhton_release *.c ../c7/stmgc.c -Wall


duhton_debug: *.c *.h $(C7SOURCES) $(C7HEADERS)
	clang -pthread -g -DDu_DEBUG -o duhton_debug *.c ../c7/stmgc.c -Wall

clean:
	rm -f duhton duhton_debug
