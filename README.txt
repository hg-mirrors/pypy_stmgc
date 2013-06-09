
STM-GC
======

Welcome!

This is a C library that combines a GC with STM capabilities.
It is meant to be a general library that can be used in C programs.

The library interface is in "c4/stmgc.h".

The file "c4/doc-objects.txt" contains some low-level explanations.

Run tests with "py.test".

A demo program will be found in "c4/demo1.c" (not there yet).
It can be built with "make debug-demo1" or "make build-demo1".

The plan is to use this C code directly with PyPy, and not write
manually the many calls to the shadow stack and the barrier functions.
But the manual way is possible too, say when writing a small interpreter
directly in C.
