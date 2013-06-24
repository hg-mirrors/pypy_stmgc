import py
import os
import subprocess

def test_and_run():
    path = os.path.dirname(__file__)
    path = os.path.dirname(path)
    res = subprocess.call(["make", "debug-demo_random"], cwd=path)
    assert not res
    res = subprocess.call(["./debug-demo_random"], cwd=path)
    assert not res
    
    
