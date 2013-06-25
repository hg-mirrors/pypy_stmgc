
all: duhton_debug duhton

duhton: *.c *.h ../c4/*.c ../c4/*.h
	gcc -lrt -pthread -g -O2 -o duhton *.c ../c4/stmgc.c -Wall

duhton_debug: *.c *.h ../c4/*.c ../c4/*.h
	gcc -lrt -pthread -g -DDu_DEBUG -D_GC_DEBUG=2 -DGC_NURSERY=2048 -o duhton_debug *.c ../c4/stmgc.c -Wall

clean:
	rm -f duhton duhton_debug
