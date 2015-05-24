Get gcc release 5.1:
    
    svn co svn://gcc.gnu.org/svn/gcc/tags/gcc_5_1_0_release

Apply the patch here.

Compile gcc as usual:
    
    mkdir ../build
    cd ../build
    ./configure --enable-stage1-languages=c
    make    # or maybe only "make all-stage1-gcc"

If you don't want to install this patched gcc globally, use this
script and call it 'gcc-seg-gs':

    #!/bin/bash
    BUILD=/..../build
    exec $BUILD/gcc/xgcc -B $BUILD/gcc "$@"
