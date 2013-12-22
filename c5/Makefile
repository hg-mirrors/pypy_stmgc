
H_FILES = core.h pagecopy.h
C_FILES = core.c pagecopy.c


demo1: demo1.c $(C_FILES) $(H_FILES)
	gcc -pthread -o $@ -O2 -g demo1.c $(C_FILES) -Wall

demo2: demo2.c largemalloc.c largemalloc.h
	gcc -o $@ -g demo2.c largemalloc.c -Wall

clean:
	rm -f demo1 demo2
