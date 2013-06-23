from support import run, evaluate


def test_make_list():
    assert run("(print (list))") == "[]\n"
    assert run("(print (list 2))") == "[ 2 ]\n"
    assert run("(print (list (+ 40 2) (- 50 1)))") == "[ 42 49 ]\n"

def test_get_list():
    assert evaluate("(get (list 20 30 40) 0)") == 20
    assert evaluate("(get (list 20 30 40) 1)") == 30
    assert evaluate("(get (list 20 30 40) 2)") == 40

def test_set_list():
    assert run("(setq l (list 20 30 40)) "
               "(set l 1 50) (print l)") == "[ 20 50 40 ]\n"

def test_append():
    assert run("(setq l (list)) "
               "(append l 10) "
               "(append l (+ 20 30)) "
               "(print l)") == "[ 10 50 ]\n"

def test_pop():
    assert run("(setq l (list 10 20 30)) "
               "(print (pop l 1)) "
               "(print l)") == "20\n[ 10 30 ]\n"
    assert run("(setq l (list 10 20 30)) "
               "(print (pop l) (pop l) (pop l))") == "30 20 10\n"

def test_len():
    assert evaluate("(len (list))") == 0
    assert evaluate("(len (list 20 30 40))") == 3
