""" A model of what occurs from the user of this library's point of
view.  The idea is that each thread creates a Revision object to start a
transaction, then asks it for reads and writes on StmObjects, and
finally calls commit_transaction().

Unlike the C equivalent, aborts are explicit: the operations on Revision
objects may raise a Conflict exception, in which case the Revision must
be forgotten and the transaction restarted.
"""

import thread


class Conflict(Exception):
    pass

class ReadWriteConflict(Conflict):
    pass

class WriteWriteConflict(Conflict):
    pass

class Deleted(Exception):
    pass


_global_lock = thread.allocate_lock()


class Revision(object):

    def __init__(self, globalstate):
        self.globalstate = globalstate
        self.previous = globalstate.most_recent_committed_revision
        self.content = {}     # mapping StmObject: [value of fields] or Deleted
        print 'MODEL: NEW rev %r' % self
        self.read_set = set()
        self.committed = False

    def __repr__(self):
        if hasattr(self, 'commit_time'):
            return '<Revision 0x%x commit_time=%r>' % (id(self),
                                                       self.commit_time)
        else:
            return '<Revision 0x%x>' % (id(self),)

    def _reverse_range(self, older_excluded_revision):
        result = []
        while self is not older_excluded_revision:
            assert self is not None
            result.append(self)
            self = self.previous
        result.reverse()
        return result

    def _extend_timestamp(self, new_previous):
        assert not self.committed
        if new_previous is self.previous:
            return False
        # first check for write-write conflicts
        c = set(self.content)
        for step in new_previous._reverse_range(self.previous):
            if c.intersection(step.content):
                raise WriteWriteConflict
        # then check for read-write conflicts
        for step in new_previous._reverse_range(self.previous):
            if self.read_set.intersection(step.content):
                raise ReadWriteConflict
        #
        self.previous = new_previous
        return True

    def _validate(self):
        gs = self.globalstate
        print 'MODEL: VALIDATE'
        self._extend_timestamp(gs.most_recent_committed_revision)

    def _commit(self, new_previous):
        self._extend_timestamp(new_previous)
        if hasattr(self, 'start_time'):
            print 'MODEL: COMMIT: start_time =', self.start_time
            #if self.start_time == 82:
            #    import pdb;pdb.set_trace()
        self.committed = True
        del self.read_set
        for stmobj in self.content:
            created_in_revision = stmobj.created_in_revision
            if created_in_revision is not self:
                past = self.previous
                while stmobj not in past.content:
                    past = past.previous
                past.content[stmobj] = Deleted
                print 'MODEL: COMMIT: DELETING %r IN %r' % (stmobj, past)

    def _add_in_read_set(self, stmobj):
        if stmobj.created_in_revision is self:
            return     # don't record local objects
        if stmobj not in self.read_set:
            print 'MODEL: ADD IN READ SET:', stmobj
            self.read_set.add(stmobj)

    def _try_read(self, stmobj):
        while stmobj not in self.content:
            self = self.previous
            assert self is not None, "reading object before its creation"
        content = self.content[stmobj]
        if content is not Deleted:
            return content
        else:
            raise Deleted

    def _try_write(self, stmobj):
        assert not self.committed
        if stmobj not in self.content:
            origin = self.previous
            while stmobj not in origin.content:
                origin = origin.previous
            content = origin.content[stmobj]
            if content is Deleted:
                raise Deleted
            self.content[stmobj] = content[:]
            print 'MODEL: TRY_WRITE: %r' % stmobj
        self._add_in_read_set(stmobj)
        return self.content[stmobj]

    def read(self, stmobj, index):
        content = self.read_barrier(stmobj)
        return content[index]

    def write(self, stmobj, index, newvalue):
        content = self.write_barrier(stmobj)
        content[index] = newvalue

    def read_barrier(self, stmobj):
        assert not self.committed
        with _global_lock:
            try:
                content = self._try_read(stmobj)
            except Deleted:
                self._validate()
                content = self._try_read(stmobj)
        self._add_in_read_set(stmobj)
        return content

    def write_barrier(self, stmobj):
        assert not self.committed
        with _global_lock:
            try:
                content = self._try_write(stmobj)
            except Deleted:
                self._validate()
                content = self._try_write(stmobj)
            return content

    def check_not_outdated(self, stmobj):
        self = self.previous
        while stmobj not in self.content:
            self = self.previous
            assert self is not None
        if self.content[stmobj] is Deleted:
            raise Deleted

    def commit_transaction(self):
        with _global_lock:
            gs = self.globalstate
            self._commit(gs.most_recent_committed_revision)
            assert self.previous is gs.most_recent_committed_revision
            gs.most_recent_committed_revision = self

    def check_can_still_commit(self):
        gs = self.globalstate
        saved = self.previous
        self._extend_timestamp(gs.most_recent_committed_revision)
        self.previous = saved


class GlobalState(object):

    def __init__(self):
        self.most_recent_committed_revision = None


class StmObject(object):

    def __init__(self, current_revision, numrefs):
        self.created_in_revision = current_revision
        current_revision.content[self] = [None] * numrefs

    def __repr__(self):
        if hasattr(self, 'identity') and hasattr(self, 'ffi'):
            return '<StmObject 0x%x>' % (
                int(self.ffi.cast('intptr_t', self.identity)),)
        else:
            return '<StmObject at 0x%x>' % (id(self),)
