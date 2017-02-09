
# STM-GC

Welcome!

This is a C library that combines a GC with STM capabilities.
It is meant to be a general library that can be used in C programs.

The library interface is in `c8/stmgc.h`.

Progress (these revisions are roughly stable versions, pick the last one):
 - 3af462f

Run tests with `py.test`.

Demo programs can be found in `c8/demo/`.

The plan is to use this C code directly with PyPy, and not write
manually the many calls to the shadow stack and the barrier functions.
But the manual way is possible too, say when writing a small interpreter
directly in C.


# Other resources

http://doc.pypy.org/en/latest/stm.html

# How to run things

## Get PyPy 

 1. `hg clone https://bitbucket.org/pypy/pypy` (this will take a while, but you
    can continue with the instructions below)
 2. `hg checkout stmgc-c8`


## Get STMGC

 1. `hg clone https://bitbucket.org/pypy/stmgc`
 2. `gcc-seg-gs/README.txt` mentions which GCC version should work. Maybe -fXXX
    flags mentioned at the end are still needed for compiling full PyPy-STM

The folder `c8` contains the current version of the STMGC library

### Project layout of c8

 - `stmgc.h`: the main header file for the library
 - `stm/`: 
    - For the GC part: 
       + `nursery`: minor collection
       + `gcpage`: major collection
       + `largemalloc`, `smallmalloc`: object allocation
       + `finalizer`: object finalizer support
       + `weakref`: weak references support
    - For the STM part:
       + `core`: commit, abort, barrier logic of STM
       + `sync`: segment management and thread support
       + `pages`: management of page metadata
       + `signal_handler`: manages pages together with `pages`
       + `locks`: a set of locks to protect segments
       + `rewind_setjmp`: setjmp/longjmp implementation that supports arbitrary rollback
       + `forksupport`: support for forking an STM process
       + `extra`: on-commit and on-abort callback mechanism
       + `detach`: transaction detach mechanism (optimised transactional zones)
    - Misc:
       + `fprintcolor`: colourful debug output
       + `hash_id`: PyPy-compatible identity and identity-hash functionality
       + `hashtable`: transactional hash table implementation
       + `queue`: transactional work-queue implementation
       + `list`: simple growable list implementation
       + `marker`, `prof`: mechanism to record events
       + `misc`: mostly debug and testing interface
       + `pagecopy`: fast copy implementation for pages
       + `prebuilt`: logic for PyPy's prebuilt objects
       + `setup`: setup code
       
       

### Running Tests

Tests are written in Python that calls the C-library through CFFI (Python package).

 1. install `pytest` and `cffi` packages for Python (via `pip`)
 2. running py.test in c8/test should run all the tests (alternatively, the
    PyPy-checkout has a pytest.py script in its root-folder, which should work
    too)

### Running Demos

Demos are small C programs that use the STMGC library directly. They sometimes
expose real data-races that the sequential Python tests cannot expose.

 1. for example: `make build-demo_random2`
 2. then run `./build-demo_random2`


## Building PyPy-STM

The STM branch of PyPy contains a *copy* of the STMGC library. After changes to
STMGC, run the `import_stmgc.py` script in `/rpython/translator/stm/`. In the
following, `/` is the root of your PyPy checkout.

 1. The Makefile expects a `gcc-seg-gs` executable to be on the `$PATH`. This
    should be a GCC that is either patched or a wrapper to GCC 6.1 that passes
    the necessary options. In my case, this is a script that points to my custom
    build of GCC with the following content:
    
    ```bash
    #!/bin/bash
    BUILD=/home/remi/work/bin/gcc-build
    exec $BUILD/gcc/xgcc -B $BUILD/gcc -fno-ivopts -fno-tree-vectorize -fno-tree-loop-distribute-patterns "$@"
    ```
    
 2. `cd /pypy/goal/`
 
 3. A script to translate PyPy-STM (adapt all paths):
 
    ```bash
    #!/bin/bash
    export PYPY_USESSION_KEEP=200
    export PYPY_USESSION_DIR=~/pypy-usession

    STM=--stm #--stm
    JIT=-Ojit #-Ojit #-O2
    VERSION=$(hg id -i)
    time  ionice -c3 pypy ~/pypy_dir/rpython/bin/rpython --no-shared --source $STM $JIT  targetpypystandalone.py
    # --no-allworkingmodules

    notify-send "PyPy" "C source generated."

    cd ~/pypy-usession/usession-$(hg branch)-remi/testing_1/
    ionice -c3 make -Bj4

    TIME=$(date +%y-%m-%d-%H:%M)
    cp pypy-c ~/pypy_dir/pypy/goal/pypy-c-$STM-$JIT-$VERSION-$TIME
    cp pypy-c ~/pypy_dir/pypy/goal/pypy-c

    notify-send "PyPy" "Make finished."
    ```
    
    The usession-folder will keep the produced C source files. You will need
    them whenever you do a change to the STMGC library only (no need to
    retranslate the full PyPy). In that case:
    
     1. Go to `~/pypy-usession/usession-stmgc-c8-$USER/testing_1/`
     2. `make clean && make -j8` will rebuild all C sources
        
        Faster alternative that works in most cases: `rm ../module_cache/*.o`
        instead of `make clean`. This will remove the compiled STMGC files,
        forcing a rebuild from the *copy* in the `/rpython/translator/stm`
        folder.
        
 4. The script puts a `pypy-c` into `/pypy/goal/` that should be ready to run.





