from support import evaluate


def test_addition():
    assert evaluate("(+ 40 2)") == 42
    assert evaluate("(+ 30 3 7 2)") == 42
    assert evaluate("(+ 30)") == 30
    assert evaluate("(+)") == 0
    assert evaluate("(+ (+ 1 2) (+ 3 4))") == 10

def test_subtraction():
    assert evaluate("(- 45 3)") == 42
    assert evaluate("(- 30 3 7 2)") == 18
    assert evaluate("(- 121)") == 121
    assert evaluate("(-)") == 0
    assert evaluate("(- 100 (- 20 1))") == 81

def test_mul():
    assert evaluate("(* 6 7)") == 42
    assert evaluate("(* 2 3 7)") == 42
    assert evaluate("(* (+ 5 1) (+ 6 1))") == 42

def test_cmp():
    assert evaluate("(<  6 6)") == 0
    assert evaluate("(<= 6 6)") == 1
    assert evaluate("(== 6 6)") == 1
    assert evaluate("(!= 6 6)") == 0
    assert evaluate("(>  6 6)") == 0
    assert evaluate("(>= 6 6)") == 1
    #
    assert evaluate("(<  6 7)") == 1
    assert evaluate("(<= 6 7)") == 1
    assert evaluate("(== 6 7)") == 0
    assert evaluate("(!= 6 7)") == 1
    assert evaluate("(>  6 7)") == 0
    assert evaluate("(>= 6 7)") == 0
    #
    assert evaluate("(<  7 6)") == 0
    assert evaluate("(<= 7 6)") == 0
    assert evaluate("(== 7 6)") == 0
    assert evaluate("(!= 7 6)") == 1
    assert evaluate("(>  7 6)") == 1
    assert evaluate("(>= 7 6)") == 1
    #
    assert evaluate("(< (+ 10 2) (+ 4 5))") == 0
