from support import run


def test_print_cons():
    assert run("(print ())") == "None\n"
    assert run("(print None)") == "None\n"
    assert run("(print (quote (1 2 3)))") == "( 1 2 3 )\n"

def test_car_cdr():
    assert run("(print (car (quote (2 3))))") == "2\n"
    assert run("(print (cdr (quote (2 3))))") == "( 3 )\n"
    assert run("(print (car (cdr (quote (2 3)))))") == "3\n"
    assert run("(print (cdr (cdr (quote (2 3)))))") == "None\n"
