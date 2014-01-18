
C7SOURCES = ../c7/core.c \
            ../c7/pagecopy.c \
            ../c7/list.c \
            ../c7/reader_writer_lock.c

all: duhton_debug duhton

duhton: *.c *.h ../c4/*.c ../c4/*.h
	clang -pthread -g -O2 -o duhton *.c $(C7SOURCES) -Wall

duhton_debug: *.c *.h ../c4/*.c ../c4/*.h
	clang -pthread -g -DDu_DEBUG -o duhton_debug *.c $(C7SOURCES) -Wall

clean:
	rm -f duhton duhton_debug
