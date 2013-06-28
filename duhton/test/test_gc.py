from support import run


def test_long1(limit=12):
    got = run("""
        (defun g (n)
         (if (>= n %d)
             (print n)
           (g (+ n 1))
           (g (+ n 2))))
        (g 0)
    """ % limit)
    pieces = got.splitlines()
    expected = []
    def g(n):
        if n >= limit:
            expected.append(str(n))
        else:
            g(n + 1)
            g(n + 2)
    g(0)
    assert pieces == expected

def test_verylong1():
    test_long1(limit=16)


def test_long_lists():
    run("""
        (defun g (lst n)
          (while (> n 30000)
            (append lst n)
            (setq n (- n 1))))
        (g (list) 34000)
    """)
    assert 1  # did not crash
