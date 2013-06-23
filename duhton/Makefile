
all: duhton_debug duhton

duhton: *.c *.h stm/*.c stm/*.h
	gcc -pthread -g -O2 -o duhton *.c stm/*.c -Wall

duhton_debug: *.c *.h stm/*.c stm/*.h
	gcc -pthread -g -DDu_DEBUG -o duhton_debug *.c stm/*.c -Wall

clean:
	rm -f duhton duhton_debug
