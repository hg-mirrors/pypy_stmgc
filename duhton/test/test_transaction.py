from support import run


def test_simple():
    assert run("(defun f (n) (print n)) "
               "(transaction f (+ 40 2)) "
               "(print (quote hello))") == "'hello'\n42\n"
    assert run("(defun f(x) (if (< x 20) (transaction f (+ x 1)) (print x))) "
               "(print (f 0))") == "None\n20\n"

def test_multiple_starts():
    got = run("""
        (defun g (n)
         (if (>= n 12)
             (print n)
           (transaction g (+ n 1))
           (transaction g (+ n 2))))
        (g 0)
    """)
    pieces = got.splitlines()
    assert len(pieces) == 377
    assert pieces.count('12') == 233
    assert pieces.count('13') == 144

def test_multiple_starts_conflicts():
    got = run("""
        (setq c (list))
        (defun g (n)
         (if (>= n 12)
             (append c n)
           (transaction g (+ n 1))
           (transaction g (+ n 2))))
        (g 0)
    """)
    assert got == ''    # how to print the final c ??  at least check no crash

def test_conflict_container():
    for i in range(20):
        res = run("""

            (setq c (container 0))

            (defun g (thread n)
                (set c (+ (get c) 1))
                (if (> (get c) 200)
                    (print (quote overflow) (get c))
                  (if (< n 100)
                      (transaction f thread (+ n 1))
                    (if (< (get c) 200)
                        (print (quote not-enough))
                      (print (quote ok))))))

            (defun f (thread n)
                (g thread n))

            (transaction f (quote t1) 1)
            (transaction f (quote t2) 1)
        
            """)
        assert res == "'not-enough'\n'ok'\n"

def test_conflict_list():
    for i in range(20):
        print 'test_conflict_list', i
        res = run("""

            (setq lst (list 0))

            (defun g (thread n)
                (set lst 0 (+ (get lst 0) 1))
                (if (> (get lst 0) 200)
                    (print (quote overflow) (get lst 0))
                  (if (< n 100)
                      (transaction f thread (+ n 1))
                    (if (< (get lst 0) 200)
                        (print (quote not-enough))
                      (print (quote ok))))))

            (defun f (thread n)
                (setq m n n -1)
                (if (== m -1)
                    (print (quote frame-has-been-modified))
                  (g thread m)))

            (transaction f (quote t1) 1)
            (transaction f (quote t2) 1)
        
            """)
        assert res == "'not-enough'\n'ok'\n"

def test_list_length():
    for i in range(20):
        print 'test_list_length', i
        res = run("""
            (setq lst (list))
            (defun f ()
                (setq n (len lst))
                (if (< n 100)
                    (transaction f)
                  (print (quote done)))
                (append lst n)
                (assert (== (len lst) (+ n 1))))
            (transaction f)
            (transaction f)
            """)
        assert res == "'done'\n'done'\n"

def test_list_pop():
    for i in range(20):
        print 'test_list_pop', i
        res = run("""
            (setq lst (list))
            (while (< (len lst) 100)
                (append lst (len lst)))
            (defun f ()
                (if (== (len lst) 0)
                    (print (quote done))
                  (assert (== (pop lst) (len lst)))
                  (transaction f)))
            (transaction f)
            (transaction f)
            """)
        assert res == "'done'\n'done'\n"

def test_list_print():
    res = run("""
        (setq lst (list 20))
        (defun f ()
            (append lst 30)
            (print lst))
        (transaction f)
        """)
    assert res == "[ 20 30 ]\n"

def test_long_lists_no_conflict():
    res = run("""
        (defun g (lst n)
          (while (> n 30000)
            (append lst n)
            (setq n (- n 1)))
          (print (len lst)))
        (transaction g (list) 34000)
        (transaction g (list) 34000)
    """)
    assert res == "4000\n4000\n"

def test_long_lists_with_conflict():
    res = run("""
        (defun g (lst n)
          (while (> n 30000)
            (append lst n)
            (setq n (- n 1)))
          (print (len lst)))
        (setq L (list))
        (transaction g L 34000)
        (transaction g L 34000)
    """)
    assert res == "4000\n8000\n"
