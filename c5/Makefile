
H_FILES = core.h pagecopy.h
C_FILES = core.c pagecopy.c

CLANG = clang -Wall -ferror-limit=3 -fno-color-diagnostics


demo1: demo1.c $(C_FILES) $(H_FILES)
	$(CLANG) -pthread -o $@ -O2 -g demo1.c $(C_FILES)

demo2: demo2.c largemalloc.c largemalloc.h
	$(CLANG) -o $@ -g demo2.c largemalloc.c

clean:
	rm -f demo1 demo2
