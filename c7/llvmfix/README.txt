
no-introduce-bogus-cast-in-combine.diff

    This is just fixes for a couple of bugs.


no-memset-creation-with-addrspace.diff

    This is a workaround for the fact that llvm.memset doesn't support
    the address_space 256.  It's a workaround, because it also prevents
    some useful optimizations: for example replacing "x->a = 0; x->b =
    0;" with a single larger zeroing instruction.  In other words, it
    crashes only if an unpatched llvm introduce llvm.memset *and* this
    memset remains as a real function call in the end.


addrspacecast-in-constant.diff

    This is a workaround for (what we believe to be) clang producing
    incorrectly the addrspacecast operation for this kind of code:

    static __attribute__((address_space(256))) long a = 42;
    struct s1 { void *a; };
    struct s1 fofo = { (void *)(long)&a };
