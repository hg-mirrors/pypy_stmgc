
========== CURRENT STATUS ==========

gcc 6.1 supports '__seg_gs' out of the box.  You should use this version
of gcc (or more recent).

If you want, you can follow the instructions below to download and
compile the standard gcc.  Of course, it is likely that gcc 6.1 will
soon be available from your Linux distribution directly.

Note that with gcc 6.1, you no longer need gcc-5.1.0-patch.diff, but you
still need the "-fno-*" options.



========== OLDER INSTRUCTIONS ==========


Get gcc release 5.1.0 from the download page:
    
    https://gcc.gnu.org/mirrors.html

Unpack it.

Apply the patch provided here in the file gcc-5.1.0-patch.diff.

You can either install the 'libmpc-dev' package on your system,
or else, manually:
    
    * unpack 'https://ftp.gnu.org/gnu/gmp/gmp-6.0.0a.tar.xz'
      and move 'gmp-6.0.0' as 'gcc-5.1.0/gmp'.

    * unpack 'http://www.mpfr.org/mpfr-current/mpfr-3.1.2.tar.xz'
      and move 'mpfr-3.1.2' as 'gcc-5.1.0/mpfr'

    * unpack 'ftp://ftp.gnu.org/gnu/mpc/mpc-1.0.3.tar.gz'
      and move 'mpc-1.0.3' as 'gcc-5.1.0/mpc'

Compile gcc as usual:
    
    mkdir build
    cd build
    ../gcc-5.1.0/configure --enable-languages=c --disable-multilib
    make    # or maybe only "make all-stage1-gcc"

This patched gcc could be globally installed, but in these instructions
we assume you don't want that.  Instead, create the following script,
call it 'gcc-seg-gs', and put it in the $PATH:

    #!/bin/bash
    BUILD=/..../build      # <- insert full path
    exec $BUILD/gcc/xgcc -B $BUILD/gcc -fno-ivopts -fno-tree-vectorize -fno-tree-loop-distribute-patterns "$@"

So far, GCC has a bug in the presence of multiple address spaces, likely
in the "ivopts" optimization.  It can be worked around by specifying
"-fno-ivopts" like above.  https://gcc.gnu.org/bugzilla/show_bug.cgi?id=66768

Another bug in -ftree-vectorize seems to generate unprefixed vector
instructions.

Similarly, GCC tries to make a memset out of 0-initializing stores and
crashes itself doing that. -fno-tree-loop-distribute-patterns 
