import py
import thread, time, sys
from __pypy__.thread import *

try:
    from pypyjit import set_param
except ImportError:
    def set_param(value):
        pass


class Conflict(Exception):
    pass


def check_no_conflict(function_list, repeat=10000):
    set_param("off")
    #
    def fn(index):
        function = function_list[index]
        sys.stdout.write("*** start %d ***\n" % index)
        reset_longest_abort_info()
        hint_commit_soon()
        i = 0
        while i < repeat:
            function()
            i += 1
        hint_commit_soon()
        abort_info = longest_abort_info()
        with atomic:
            abort_infos.append(abort_info)
            if len(abort_infos) == count:
                finished.release()
    #
    abort_infos = []
    finished = thread.allocate_lock()
    finished.acquire()
    count = len(function_list)
    tlist = [thread.start_new_thread(fn, (i,)) for i in range(count)]
    finished.acquire()
    for i in range(count):
        print 'thread %d: %r' % (i, abort_infos[i])
    if abort_infos != [None] * count:
        raise Conflict

def check_conflict(*args, **kwds):
    py.test.raises(Conflict, check_no_conflict, *args, **kwds)
