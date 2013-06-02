
STM-GC
======

Welcome!

This is a C library that combines a GC with STM capabilities.
It is meant to be a general library that can be used in C programs.

The library interface is in "c3/stmgc.h".

The file "c3/doc-stmgc.txt" contains a high-level overview followed by
more detailled explanations.

A demo program can be found in "c3/demo1.c", but the code so far is
outdated (it doesn't follow what c3/doc-stmgc describes).
It can be built with "make debug-demo1" or "make build-demo1".

The plan is to use this C code directly with PyPy, and not write
manually the many calls to the shadow stack and the barrier functions.
But the manual way is possible too, say when writing a small interpreter
directly in C.
