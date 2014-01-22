
C7SOURCES = ../c7/core.c \
            ../c7/pagecopy.c \
            ../c7/list.c \
            ../c7/pages.c \
            ../c7/nursery.c \
            ../c7/stmsync.c \
            ../c7/reader_writer_lock.c

C7HEADERS = ../c7/*.h


all: duhton_debug duhton

duhton: *.c *.h $(C7SOURCES) $(C7HEADERS)
	clang -pthread -g -O2 -o duhton *.c $(C7SOURCES) -Wall

duhton_debug: *.c *.h $(C7SOURCES) $(C7HEADERS)
	clang -pthread -g -DDu_DEBUG -o duhton_debug *.c $(C7SOURCES) -Wall

clean:
	rm -f duhton duhton_debug
