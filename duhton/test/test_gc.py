from support import run


def test_long1():
    got = run("""
        (defun g (n)
         (if (>= n 7)
             (print n)
           (g (+ n 1))
           (g (+ n 2))))
        (g 0)
    """)
    pieces = got.splitlines()
    assert len(pieces) == xxx
    assert 0
