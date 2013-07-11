from support import run


def test_print_cons():
    assert run("(print ())") == "None\n"
    assert run("(print None)") == "None\n"
    assert run("(print (quote (1 2 3)))") == "( 1 2 3 )\n"
    assert run("(print (cons 1 2))") == "( 1 . 2 )\n"

def test_pair():
    assert run("(print (pair? 1))") == "0\n"
    assert run("(print (pair? (cons 1 2)))") == "1\n"
    assert run("(setq x (cons 1 2)) (print (pair? x))") == "1\n"
    assert run("(setq x 42) (print (pair? x))") == "0\n"

def test_car_cdr():
    assert run("(print (car (quote (2 3))))") == "2\n"
    assert run("(print (cdr (quote (2 3))))") == "( 3 )\n"
    assert run("(print (car (cdr (quote (2 3)))))") == "3\n"
    assert run("(print (cdr (cdr (quote (2 3)))))") == "None\n"
