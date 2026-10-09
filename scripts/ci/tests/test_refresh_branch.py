import pathlib
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
import refresh_branch
from generated_blocks import digest  # refresh_branch puts launcher/tools/render on the path

TABLE = "| a |\n|---|\n| {} |\n"


def git(*args, cwd):
    return subprocess.run(["git", "-c", "user.name=t", "-c", "user.email=t@t", *args], cwd=cwd, check=True,
                          capture_output=True, text=True).stdout


def write(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8", newline="\n")
    return path


def block(name, body):
    body = "\n" + body
    return f"<!-- generated: {name} sha256={digest(body)} -->{body}<!-- /generated: {name} -->\n"


class RefreshBranchTests(unittest.TestCase):
    """A run that started on an older main, finishing after main moved its table elsewhere."""

    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        base = pathlib.Path(temp.name)
        self.remote, self.main, self.run, self.tables = base / "remote.git", base / "main", base / "run", base / "tables"
        git("init", "-q", "--bare", "-b", "main", str(self.remote), cwd=base)
        git("clone", "-q", str(self.remote), str(self.main), cwd=base)
        write(self.main / "docs/old.md", "# Old\n\n" + block("t", TABLE.format(1)))
        write(self.main / "docs/images/a.png", "old pixels")
        write(self.main / "docs/images/gone.png", "unused")
        git("add", "-A", cwd=self.main)
        git("commit", "-q", "-m", "start", cwd=self.main)
        git("push", "-q", "origin", "main", cwd=self.main)
        git("clone", "-q", str(self.remote), str(self.run), cwd=base)
        # main moves the table to another document after the run started
        write(self.main / "docs/old.md", "# Old\n\nThe table moved.\n")
        write(self.main / "docs/new.md", "# New\n\n" + block("t", TABLE.format(1)))
        git("add", "-A", cwd=self.main)
        git("commit", "-q", "-m", "move the table", cwd=self.main)
        git("push", "-q", "origin", "main", cwd=self.main)
        # the run's own results, written into its stale checkout
        write(self.tables / "t.md", TABLE.format(2))
        write(self.run / "docs/images/a.png", "new pixels")
        (self.run / "docs/images/gone.png").unlink()
        write(self.run / "docs/old.md", "# Old\n\n" + block("t", TABLE.format(2)))

    def branch_file(self, path):
        return git("show", f"refresh:{path}", cwd=self.remote)

    def refresh(self, images=("docs/images/a.png", "docs/images/gone.png")):
        return refresh_branch.refresh(self.run, "refresh", "refresh", self.tables, list(images))

    def test_the_table_goes_where_main_holds_it_now(self):
        self.assertTrue(self.refresh())
        self.assertIn(TABLE.format(2), self.branch_file("docs/new.md"))
        self.assertEqual(self.branch_file("docs/old.md"), "# Old\n\nThe table moved.\n")

    def test_the_branch_is_main_plus_the_run_s_images(self):
        self.refresh()
        self.assertEqual(self.branch_file("docs/images/a.png"), "new pixels")
        self.assertNotIn("docs/images/gone.png", git("ls-tree", "-r", "--name-only", "refresh", cwd=self.remote))
        main = git("rev-parse", "main", cwd=self.remote).strip()
        self.assertEqual(git("rev-parse", "refresh~1", cwd=self.remote).strip(), main)

    def test_nothing_is_pushed_when_main_already_matches_the_run(self):
        write(self.tables / "t.md", TABLE.format(1))
        self.assertFalse(self.refresh(images=()))
        self.assertEqual(git("branch", "--list", "refresh", cwd=self.remote), "")


if __name__ == "__main__":
    unittest.main()
