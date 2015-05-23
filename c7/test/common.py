import os
import sys
assert sys.maxint == 9223372036854775807, "requires a 64-bit environment"

# ----------
os.environ['CC'] = './gcc-patched'

parent_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# ----------

source_files = [os.path.join(parent_dir, "stmgc.c")]
all_files = [os.path.join(parent_dir, "stmgc.h"),
             os.path.join(parent_dir, "stmgc.c")] + [
    os.path.join(parent_dir, 'stm', _n)
        for _n in os.listdir(os.path.join(parent_dir, 'stm'))
                 if (_n.endswith('.h') or _n.endswith('.c')) and not _n.startswith('.')]

_pycache_ = os.path.join(parent_dir, 'test', '__pycache__')
if os.path.exists(_pycache_):
    _fs = [_f for _f in os.listdir(_pycache_) if _f.startswith('_cffi_')]
    if _fs:
        _fsmtime = min(os.stat(os.path.join(_pycache_, _f)).st_mtime
                       for _f in _fs)
        if any(os.stat(src).st_mtime >= _fsmtime for src in all_files):
            import shutil
            shutil.rmtree(_pycache_)
