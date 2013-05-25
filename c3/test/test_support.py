import py
from support import *


def test_run_parallel():
    def f1(r):
        lst.append("a")
        r.set(1)
        r.wait(2)
        lst.append("c")
    def f2(r):
        r.wait(1)
        lst.append("b")
        r.set(2)
    lst = []
    run_parallel(f1, f2)
    assert lst == ["a", "b", "c"]

def test_run_parallel_exception():
    class FooError(Exception):
        pass
    def f1(r):
        raise FooError # :-)
    e = py.test.raises(FooError, run_parallel, f1)
    assert str(e.traceback[-1].statement).endswith('raise FooError # :-)')

def test_run_parallel_exception_bug():
    class FooError(Exception):
        pass
    def f1(r):
        raise FooError # :-)
    def f2(r):
        pass
    e = py.test.raises(FooError, run_parallel, f1, f2)
    assert str(e.traceback[-1].statement).endswith('raise FooError # :-)')

def test_run_parallel_exception_dont_deadlock():
    class FooError(Exception):
        pass
    def f1(r):
        raise FooError # :-)
    def f2(r):
        r.wait(2)
    e = py.test.raises(FooError, run_parallel, f1, f2)
    assert str(e.traceback[-1].statement).endswith('raise FooError # :-)')
