import os
import sys
assert sys.maxint == 9223372036854775807, "requires a 64-bit environment"

# ----------
os.environ['CC'] = 'clang'

parent_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
