import py
import os
import subprocess

def test_and_run_debug():
    path = os.path.dirname(__file__)
    path = os.path.dirname(path)
    res = subprocess.call(["make", "debug-demo_random"], cwd=path)
    assert not res
    res = subprocess.call(["./debug-demo_random"], cwd=path)
    assert not res
    
def test_and_run_build():
    path = os.path.dirname(__file__)
    path = os.path.dirname(path)
    res = subprocess.call(["make", "build-demo_random"], cwd=path)
    assert not res
    res = subprocess.call(["./build-demo_random"], cwd=path)
    assert not res

def test_and_run_release():
    path = os.path.dirname(__file__)
    path = os.path.dirname(path)
    res = subprocess.call(["make", "release-demo_random"], cwd=path)
    assert not res
    res = subprocess.call(["./release-demo_random"], cwd=path)
    assert not res

