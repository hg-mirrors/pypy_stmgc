
H_FILES = core.h pagecopy.h
C_FILES = core.c pagecopy.c


demo1: demo1.c $(C_FILES) $(H_FILES)
	gcc -o $@ -O2 -g demo1.c $(C_FILES) -Wall

clean:
	rm -f demo1
