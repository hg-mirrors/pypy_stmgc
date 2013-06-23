from support import run, evaluate


def test_type():
    assert run("(print (type 42))") == "<type 'int'>\n"
    assert run("(print (type (+ 40 2)))") == "<type 'int'>\n"

def test_quote():
    assert run("(print (quote (+ 40 2)))") == "( '+' 40 2 )\n"
    assert run("(print (type (quote (+ 40 2))))") == "<type 'cons'>\n"

def test_if():
    assert evaluate("(if 5 6 7)") == 6
    assert evaluate("(if 0 6 7)") == 7
    assert run("(if (- 2 3) (print 6))") == "6\n"
    assert run("(if (- 3 3) (print 6))") == ""
    assert run("(if (- 3 3) (print 6) (print 7) (print 8))") == "7\n8\n"

def test_while():
    assert run("(setq n 5) "
               "(while n (print n) (setq n (- n 1)))") == "5\n4\n3\n2\n1\n"
    assert run("(setq n 5) "
               "(while (>= n 2) (print n) (setq n (- n 1)))") == "5\n4\n3\n2\n"

def test_true_false():
    assert evaluate("(if None 6 7)") == 7
    assert evaluate("(if (quote (2)) 6 7)") == 6
    assert evaluate("(if (quote foo) 6 7)") == 6
    assert evaluate("(if (list 42) 6 7)") == 6
    assert evaluate("(if (list) 6 7)") == 7

def test_not():
    assert evaluate("(not 0)") == 1
    assert evaluate("(not 1)") == 0
    assert evaluate("(not -2)") == 0
    assert evaluate("(not None)") == 1
    assert evaluate("(not (list))") == 1
    assert evaluate("(not (list 42))") == 0
    assert evaluate("(not (quote (2)))") == 0
    assert evaluate("(not (quote xx))") == 0
    assert evaluate("(not (type 5))") == 0

def test_comment():
    assert evaluate(";42\n6") == 6

def test_defined():
    assert evaluate("(defined? abcdefghijk)") == 0
    assert run("(setq abcd 42) (print (defined? abcd))") == "1\n"
    assert run("(defun f (x) (defined? x)) (print (f 42))") == "1\n"

def test_assert():
    assert run("(assert (== (+ 40 2) 42))") == ""
