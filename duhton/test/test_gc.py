from support import run


def test_long1():
    got = run("""
        (defun g (n)
         (if (>= n 12)
             (print n)
           (g (+ n 1))
           (g (+ n 2))))
        (g 0)
    """)
    pieces = got.splitlines()
    expected = []
    def g(n):
        if n >= 12:
            expected.append(str(n))
        else:
            g(n + 1)
            g(n + 2)
    g(0)
    assert pieces == expected
