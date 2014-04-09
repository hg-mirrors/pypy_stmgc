""" Adds two built-in functions: $rfs(p=0) and $rgs(p=0).

Returns the number or the address 'p', offset with the value of
the %fs or %gs register in the current thread.

Usage: you can for example add this line in your ~/.gdbinit:

    python execfile('/path/to/gdb_stm.py')
"""
import gdb

def gdb_function(func):
    class Func(gdb.Function):
        __doc__ = func.__doc__
        invoke = staticmethod(func)
    Func(func.__name__)

# -------------------------------------------------------

SEG_FS = 0x1003
SEG_GS = 0x1004

def get_segment_register(which):
    v = gdb.parse_and_eval('(long*)malloc(8)')
    L = gdb.lookup_type('long')
    gdb.parse_and_eval('arch_prctl(%d, %d)' % (which, int(v.cast(L))))
    result = int(v.dereference())
    gdb.parse_and_eval('free(%d)' % (int(v.cast(L)),))
    return result

def rfsrgs(name, which):
    seg = get_segment_register(which)
    if name is None:
        return seg
    tp = name.type
    if tp.code == gdb.TYPE_CODE_INT:
        return name + seg
    assert tp.code == gdb.TYPE_CODE_PTR
    L = gdb.lookup_type('long')
    return (name.cast(L) + seg).cast(tp)

@gdb_function
def rfs(name=None):
    return rfsrgs(name, SEG_FS)

@gdb_function
def rgs(name=None):
    return rfsrgs(name, SEG_GS)

