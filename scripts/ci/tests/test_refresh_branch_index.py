"""refresh_branch's figure_index hook commits what the figure index generator makes beside the doc images
that changed, and nothing else."""
import os
import pathlib
import subprocess
import sys
import tempfile
import textwrap
import unittest
from unittest import mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
import refresh_branch  # noqa: E402
from test_refresh_branch import git, write  # noqa: E402

THUMBNAIL = f"{refresh_branch.THUMBNAILS}/anim/spin.gif"
ORPHAN = f"{refresh_branch.THUMBNAILS}/old/orphan.gif"
# The generator: writes the page and a new thumbnail, removes the orphan thumbnail, in the tree it runs in.
GENERATOR = f'''
import pathlib, sys
root = pathlib.Path.cwd()
(root / "{refresh_branch.PAGE}").write_text("new page " + " ".join(sys.argv[1:]), encoding="utf-8")
(root / "{THUMBNAIL}").parent.mkdir(parents=True, exist_ok=True)
(root / "{THUMBNAIL}").write_text("new thumbnail", encoding="utf-8")
(root / "{ORPHAN}").unlink()
'''


class FigureIndexHookTests(unittest.TestCase):
    def setUp(self):
        environment = mock.patch.dict(os.environ, GIT_CONFIG_GLOBAL=os.devnull, GIT_CONFIG_NOSYSTEM="1")
        environment.start()
        self.addCleanup(environment.stop)
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        base = pathlib.Path(temp.name)
        self.remote, self.run, self.generator = base / "remote.git", base / "run", base / "generator.py"
        git("init", "-q", "--bare", "-b", "main", str(self.remote), cwd=base)
        git("clone", "-q", str(self.remote), str(self.run), cwd=base)
        write(self.run / "docs/images/a.png", "old pixels")
        write(self.run / refresh_branch.PAGE, "old page")
        write(self.run / ORPHAN, "orphan thumbnail")
        git("add", "-A", cwd=self.run)
        git("commit", "-q", "-m", "start", cwd=self.run)
        git("push", "-q", "origin", "main", cwd=self.run)
        write(self.run / "docs/images/a.png", "new pixels")
        write(self.run / "docs/untracked.txt", "scratch")
        write(self.run / "docs/tools/scratch.txt", "scratch")
        write(self.generator, textwrap.dedent(GENERATOR))
        patch = mock.patch.object(refresh_branch, "WRITE", f"python {self.generator} index")
        patch.start()
        self.addCleanup(patch.stop)

    def refresh(self, images=("docs/images/a.png",)):
        return refresh_branch.refresh(self.run, "refresh", "refresh", None, list(images),
                                      derive=refresh_branch.figure_index)

    def pushed_files(self):
        return git("ls-tree", "-r", "--name-only", "refresh", cwd=self.remote).split()

    def test_the_page_a_new_thumbnail_and_a_removed_orphan_land_in_the_images_commit(self):
        self.assertTrue(self.refresh())
        self.assertEqual(git("show", f"refresh:{refresh_branch.PAGE}", cwd=self.remote), "new page index")
        self.assertEqual(git("show", f"refresh:{THUMBNAIL}", cwd=self.remote), "new thumbnail")
        self.assertEqual(git("show", "refresh:docs/images/a.png", cwd=self.remote), "new pixels")
        self.assertNotIn(ORPHAN, self.pushed_files())
        self.assertEqual(git("rev-list", "--count", "main..refresh", cwd=self.remote).strip(), "1")

    def test_an_unrelated_untracked_file_under_docs_is_not_committed(self):
        self.refresh()
        self.assertEqual([name for name in self.pushed_files() if "scratch" in name or "untracked" in name], [])
        self.assertTrue((self.run / "docs/untracked.txt").is_file())

    def test_a_generator_that_fails_raises_and_pushes_nothing(self):
        write(self.generator, "import sys\nsys.exit(1)\n")
        with self.assertRaises(subprocess.CalledProcessError):
            self.refresh()
        self.assertEqual(git("branch", "--list", "refresh", cwd=self.remote), "")

    def test_main_hands_the_hook_to_refresh_as_derive(self):
        with mock.patch.object(refresh_branch, "refresh", return_value=True) as refresh:
            self.assertEqual(refresh_branch.main(["refresh", "message", "docs/images/a.png"]), 0)
        args, kwargs = refresh.call_args
        derive = args[6] if len(args) > 6 else kwargs.get("derive")
        self.assertIs(derive, refresh_branch.figure_index)

    def test_main_exits_3_when_there_was_nothing_to_commit(self):
        with mock.patch.object(refresh_branch, "refresh", return_value=False):
            self.assertEqual(refresh_branch.main(["refresh", "message"]), refresh_branch.UNCHANGED)


if __name__ == "__main__":
    unittest.main()
