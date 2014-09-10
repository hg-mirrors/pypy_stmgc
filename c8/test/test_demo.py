import py
import os


class TestDemo:

    def _do(self, cmd):
        print cmd
        err = os.system(cmd)
        if err: py.test.fail("'%s' failed (result %r)" % (cmd, err))

    def make_and_run(self, target):
        self._do("make -C ../demo %s" % target)
        self._do("../demo/%s > /dev/null" % target)

    def test_shadowstack(self):
        py.test.xfail("no major gc yet")
        self.make_and_run("debug-test_shadowstack")

    def test_demo_simple_build(self):   self.make_and_run("build-demo_simple")

    def test_demo_largemalloc_build(self):
        py.test.xfail("no largemalloc")
        self.make_and_run("build-demo_largemalloc")



    # def test_demo2_debug(self):   self.make_and_run("debug-demo2")
    def test_demo2_build(self):
        py.test.xfail("no markers yet")
        self.make_and_run("build-demo2")
    # def test_demo2_release(self): self.make_and_run("release-demo2")

    # def test_demo_random_debug(self):   self.make_and_run("debug-demo_random")
    def test_demo_random_build(self):   self.make_and_run("build-demo_random")
    def test_demo_random_release(self): self.make_and_run("release-demo_random")

    # def test_demo_random2_debug(self):   self.make_and_run("debug-demo_random2")
    def test_demo_random2_build(self):   self.make_and_run("build-demo_random2")
    def test_demo_random2_release(self): self.make_and_run("release-demo_random2")
