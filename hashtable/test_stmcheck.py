import stmcheck


def test_no_conflict():
    def t1():
        pass
    def t2():
        pass
    stmcheck.check_no_conflict([t1, t2])

def test_obvious_conflict():
    lst = [0]
    def t1():
        lst[0] += 1
    stmcheck.check_conflict([t1, t1])

def test_no_conflict_if_writing_to_different_lists():
    lst = [[0], [0]]
    def t1():
        lst[0][0] += 1
    def t2():
        lst[1][0] += 1
    stmcheck.check_no_conflict([t1, t2])

def test_conflict_even_if_writing_to_different_offsets():
    lst = [0, 0]
    def t1():
        lst[0] += 1
    def t2():
        lst[1] += 1
    stmcheck.check_conflict([t1, t2])
