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

def test_div_mod():
    assert evaluate("(/ 11 2)") == 5
    assert evaluate("(/ 29 2 3)") == 4
    assert evaluate("(% 29 2)") == 1
    assert evaluate("(% 29 10 3)") == 0

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

def test_and_or():
    assert evaluate("(&& 1 1 1)") == 1
    assert evaluate("(&& 1 0 1)") == 0
    assert evaluate("(&& 0 sdfdsfsd)") == 0
    assert evaluate("(&& None)") == 0
    assert evaluate("(&& (quote bla))") == 1
    assert evaluate("(&& )") == 1
    
    assert evaluate("(|| 0 1)") == 1
    assert evaluate("(|| 0 0 0 1)") == 1
    assert evaluate("(|| 0 0 0)") == 0
    assert evaluate("(|| 1 sdfdsfafds)") == 1
    assert evaluate("(|| None)") == 0
    assert evaluate("(|| (quote bla))") == 1
    assert evaluate("(|| )") == 0
    
    
def test_shifts_bitwise():
    assert evaluate("(<< 1 1)") == 2
    assert evaluate("(<< 12)") == 12
    assert evaluate("(<< 1 1 1)") == 4
    assert evaluate("(<< 0 1)") == 0
    
    assert evaluate("(>> 4 1 1)") == 1
    assert evaluate("(>> 4 3)") == 0
    assert evaluate("(>> 4)") == 4

    assert evaluate("(^ 1 4)") == 1 ^ 4
    assert evaluate("(^ 1 4 122)") == 1 ^ 4 ^ 122
    assert evaluate("(^ 1)") == 1
    assert evaluate("(^)") == 0
