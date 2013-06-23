from support import run, evaluate


def test_setq():
    assert evaluate("(setq foo 42)") == 42
    assert evaluate("(setq)") == None
    assert evaluate("(setq foo 42 bar 84)") == 84
    assert evaluate("(setq foo (+ 40 2))") == 42
    #
    assert run("(setq foo 42) (print foo)") == '42\n'
    assert run("(setq foo 42 bar 84) (print foo)") == '42\n'
    #
    assert run("(setq foo 40) (setq foo (+ foo 2)) (print foo)") == '42\n'

def test_defun():
    assert run("(defun foo () 42)") == ''
    assert run("(defun foo (a b) (+ a b))") == ''
    #
    assert run("(defun foo () 42) (print (foo))") == '42\n'
    assert run("(defun foo (a b) (+ a b)) (print (foo 20 22))") == '42\n'
    assert run("(defun foo (a b) (+ a b)) (print (foo (foo 1 2) 3))") == '6\n'
