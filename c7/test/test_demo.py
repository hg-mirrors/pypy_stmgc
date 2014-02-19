import py
import os


class TestDemo:

    def _do(self, cmd):
        print cmd
        err = os.system(cmd)
        if err: py.test.fail("'%s' failed (result %r)" % (cmd, err))

    def make_and_run(self, target):
        self._do("make -C ../demo %s" % target)
        self._do("../demo/%s" % target)

    def test_demo2_debug(self):   self.make_and_run("debug-demo2")
    def test_demo2_build(self):   self.make_and_run("build-demo2")
    def test_demo2_release(self): self.make_and_run("release-demo2")
