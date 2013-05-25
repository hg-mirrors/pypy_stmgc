import py
from model import *


def test_simple():
    gs = GlobalState()

    rev0 = Revision(gs)                                 #
    obj0 = StmObject(rev0, 1)                           #
    rev0.write(obj0, 0, None)                           #
    rev0.commit_transaction()                           #

    rev1 = Revision(gs)                                 # 1
    assert rev1.previous is rev0                        # 1
    obj1 = StmObject(rev1, 2)                           # 1

    rev2 = Revision(gs)                                     # 2
    assert rev2.previous is rev0                            # 2
    obj2 = StmObject(rev2, 3)                               # 2

    rev1.write(obj0, 0, obj1)                           # 1
    rev1.commit_transaction()                           # 1
    assert gs.most_recent_committed_revision is rev1    # 1

    assert rev2.read(obj2, 0) is None                       # 2
    assert rev2.previous is rev0                            # 2
    assert rev2.read(obj0, 0) is obj1                       # 2
    assert rev2.previous is rev1                            # 2
    rev2.write(obj2, 0, obj1)                               # 2
    rev2.write(obj1, 1, obj2)                               # 2
    assert rev2.read(obj2, 0) is obj1                       # 2

    rev3 = Revision(gs)                                 # 1
    assert rev3.read(obj1, 1) is None                   # 1

    rev2.commit_transaction()                               # 2
    py.test.raises(Conflict, rev3.read, obj1, 1)        # 1
