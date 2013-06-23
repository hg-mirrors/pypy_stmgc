from support import run, evaluate


def test_make_container():
    assert run("(print (container))") == "<container None>\n"
    assert run("(print (container 2))") == "<container 2>\n"
    assert run("(print (container (+ 40 2)))") == "<container 42>\n"

def test_get_container():
    assert evaluate("(get (container 20))") == 20

def test_set_container():
    assert run("(setq c (container)) "
               "(set c 50) (print c)") == "<container 50>\n"
