
STM-GC
======

Welcome!

This is a C library that combines a GC with STM capabilities.
It is meant to be a general library that can be used in C programs.

The library interface is in "c4/stmgc.h".

Progress (these revisions are roughly stable versions, pick the last one):
- 3aea86a96daf: last rev of "c3", the previous version
- f1ccf5bbcb6f: first step, working but with no GC
- 8da924453f29: minor collection seems to be working, no major GC
- e7249873dcda: major collection seems to be working

The file "c4/doc-objects.txt" contains some low-level explanations.

Run tests with "py.test".

A demo program can be found in "c4/demo1.c".
It can be built with "make debug-demo1" or "make build-demo1".

The plan is to use this C code directly with PyPy, and not write
manually the many calls to the shadow stack and the barrier functions.
But the manual way is possible too, say when writing a small interpreter
directly in C.
