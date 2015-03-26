"""Adds a few built-in functions for stmgc-c7:

 $gc(p=0, thread='current')

    With an integer argument `p`, returns `(char *)(segment_base + p)`.
    If `p` is a pointer type, it is assumed to be %gs-relative; the
    function returns `*p` in the segment.

    The segment is by default the current one, as computed by looking
    around in the debug information.  You can force with the second
    argment a specific segment number or a specific pthread_self()
    thread address.

 $psegment(thread='current')

    Return the 'stm_priv_segment_info_t *' of the given thread
    (given as a segment number or a pthread_self() thread address).

Usage: you can for example add this line in your ~/.gdbinit:

    python exec(open('/path/to/gdb_stm.py').read())
"""
import gdb

def gdb_function(func):
    class Func(gdb.Function):
        __doc__ = func.__doc__
        invoke = staticmethod(func)
    Func(func.__name__)

# -------------------------------------------------------

_nb_segments = None
_segment_size = None
_psegment_ofs = None

def get_nb_segments():
    global _nb_segments
    if _nb_segments is None:
        _nb_segments = int(gdb.parse_and_eval('_stm_nb_segments'))
        assert 1 < _nb_segments <= 240
    return _nb_segments

def get_segment_size():
    global _segment_size
    if _segment_size is None:
        nb_pages = int(gdb.parse_and_eval('_stm_segment_nb_pages'))
        _segment_size = nb_pages * 4096
    return _segment_size

def get_psegment_ofs():
    global _psegment_ofs
    if _psegment_ofs is None:
        _psegment_ofs = int(gdb.parse_and_eval('_stm_psegment_ofs'))
    return _psegment_ofs

def get_segment_base(segment_id):
    assert 0 <= segment_id <= get_nb_segments()
    base = int(gdb.parse_and_eval('stm_object_pages'))
    return base + get_segment_size() * segment_id

def get_psegment(segment_id, field=''):
    assert 0 < segment_id <= get_nb_segments()
    return gdb.parse_and_eval(
        '((struct stm_priv_segment_info_s *)(stm_object_pages+%d))%s'
        % (get_segment_size() * segment_id + get_psegment_ofs(), field))

def thread_to_segment_id(thread_id):
    base = int(gdb.parse_and_eval('stm_object_pages'))
    for j in range(1, get_nb_segments() + 1):
        ts = get_psegment(j, '->transaction_state')
        if int(ts) != 0:
            ti = get_psegment(j, '->pub.running_thread->creating_pthread[0]')
            if int(ti) == thread_id:
                return j
    raise Exception("thread not found: %r" % (thread_id,))

def interactive_segment_base(thread=None):
    if thread is None:
        s = gdb.execute('info threads', False, True)
        i = s.find('\n* ')
        assert i >= 0
        fields = s[i+2:].split()
        assert fields[1] == 'Thread'
        assert fields[2].startswith('0x')
        thread_id = int(fields[2], 16)
        segment_id = thread_to_segment_id(thread_id)
    elif thread.type.code == gdb.TYPE_CODE_INT:
        if 0 <= int(thread) < 256:
            segment_id = int(thread)
        else:
            thread_id = int(thread)
            segment_id = thread_to_segment_id(thread_id)
    else:
        raise TypeError("'thread' argument must be an int or not given")
    return get_segment_base(segment_id)

@gdb_function
def gc(p=None, thread=None):
    sb = interactive_segment_base(thread)
    if p is not None and p.type.code == gdb.TYPE_CODE_PTR:
        return gdb.Value(sb + int(p)).cast(p.type).dereference()
    elif p is None or int(p) == 0:
        T = gdb.lookup_type('char').pointer()
        return gdb.Value(sb).cast(T)
    else:
        raise TypeError("gc() first argument must be a GC pointer or 0")

@gdb_function
def psegment(thread=None):
    sb = interactive_segment_base(thread)
    return gdb.parse_and_eval(
        '*((struct stm_priv_segment_info_s *)%d)'
        % (sb + get_psegment_ofs(),))
