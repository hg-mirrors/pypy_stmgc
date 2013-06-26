import py, re
import os, subprocess
from cStringIO import StringIO


_compiled = False

def _do(cmdargs, stdin=''):
    popen = subprocess.Popen(cmdargs,
                             stdin  = subprocess.PIPE,
                             stdout = subprocess.PIPE,
                             stderr = subprocess.PIPE)
    popen.stdin.write(stdin)
    popen.stdin.close()
    result = popen.stdout.read()
    error = popen.stderr.read()
    exitcode = popen.wait()
    if exitcode:
        raise OSError("%r failed (exit code %r)\n" % (cmdargs[0], exitcode) +
                      error.rstrip())
    if filter_out_colored_output(error):
        raise OSError("%r got on stderr:\n" % (cmdargs[0],) + error.rstrip())
    return result

def filter_out_colored_output(text):
    return re.sub('\x1b\\[3.m[\w\W]*?\x1b\\[0m', '', text)


def execute(argv, stdin=''):
    global _compiled
    dir = py.path.local()
    if dir.basename == 'test':
        dir = dir.dirpath()
    if not _compiled:
        _do(["make", "-C", str(dir)])
        _compiled = True
    arg = str(dir.join('duhton_debug'))
    return _do([arg] + argv, stdin)


def run(filecontent):
    return execute(['-'], stdin=filecontent)

def evaluate(expression):
    text = execute(['-'], stdin='(print %s)' % expression)
    return eval(text)
