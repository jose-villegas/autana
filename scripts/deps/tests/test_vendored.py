"""A submodule bump fails the check until update merges upstream's change into the copy."""
import contextlib
import io
import pathlib
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
import vendored

GIT = ["git", "-c", "user.name=t", "-c", "user.email=t@t", "-c", "protocol.file.allow=always",
       "-c", "core.autocrlf=false"]


def run(cwd, *args):
    return subprocess.run([*GIT, *args], cwd=cwd, check=True, capture_output=True, text=True).stdout.strip()


def commit(repo, files, message):
    for name, text in files.items():
        path = repo / name
        if text is None:
            path.unlink()
            continue
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, newline="\n")
    run(repo, "add", "-A")
    run(repo, "commit", "-qm", message)
    return run(repo, "rev-parse", "HEAD")


class VendoredTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        base = pathlib.Path(self.directory.name).resolve()
        self.upstream = base / "upstream"
        self.upstream.mkdir()
        run(self.upstream, "init", "-q")
        self.first = commit(self.upstream, {"lib/core.c": "one\ntwo\nthree\nfour\nfive\n",
                                            "lib/core.h": "header\n", "lib/gui.c": "gui\n"}, "first")
        self.root = base / "project"
        self.root.mkdir()
        run(self.root, "init", "-q")
        run(self.root, "submodule", "add", "-q", str(self.upstream), "third_party/up")
        copy = self.root / "vendor"
        (copy / "include").mkdir(parents=True)
        # The copy cuts gui.c, moves the header and edits its first line.
        (copy / "core.c").write_text("ONE (ours)\ntwo\nthree\nfour\nfive\n", newline="\n")
        (copy / "include" / "core.h").write_text("header\n", newline="\n")
        commit(self.root, {}, "vendor")
        self.entries = {"vendor": {"upstream": "third_party/up", "from": "lib",
                                   "moved": {"core.h": "include/core.h"}, "taken_from": self.first}}
        self.registry = self.root / "vendored.json"

    def tearDown(self):
        self.directory.cleanup()

    def bump(self, files):
        new = commit(self.upstream, files, "upstream change")
        sub = self.root / "third_party" / "up"
        run(sub, "fetch", "-q", "origin")
        run(sub, "checkout", "-q", new)
        run(self.root, "add", "third_party/up")
        return new

    def check(self):
        with contextlib.redirect_stderr(io.StringIO()) as err:
            code = vendored.check(self.entries, self.root)
        return code, err.getvalue()

    def update(self):
        with contextlib.redirect_stdout(io.StringIO()) as out:
            code = vendored.update(self.entries, self.registry, self.root)
        return code, out.getvalue()

    def test_a_copy_taken_from_the_pin_passes(self):
        self.assertEqual(self.check(), (0, ""))

    def test_a_moved_pin_fails_naming_the_update_command(self):
        new = self.bump({"lib/core.c": "one\ntwo\nthree\nfour\nFIVE\n"})
        code, message = self.check()
        self.assertEqual(code, 1)
        self.assertIn(new[:12], message)
        self.assertIn("scripts/deps/vendored.py update", message)

    def test_update_merges_upstream_and_keeps_our_edits(self):
        new = self.bump({"lib/core.c": "one\ntwo\nthree\nfour\nFIVE\n", "lib/core.h": "header v2\n",
                         "lib/gui.c": "gui v2\n"})
        code, report = self.update()
        self.assertEqual(code, 0, report)
        self.assertEqual((self.root / "vendor/core.c").read_text(), "ONE (ours)\ntwo\nthree\nfour\nFIVE\n")
        self.assertEqual((self.root / "vendor/include/core.h").read_text(), "header v2\n")
        self.assertFalse((self.root / "vendor/gui.c").exists())
        self.assertIn("cut from the copy", report)
        self.assertEqual(vendored.load(self.registry)["vendor"]["taken_from"], new)
        self.assertEqual(self.check(), (0, ""))

    def test_a_conflict_is_left_marked_and_the_check_fails_until_resolved(self):
        new = self.bump({"lib/core.c": "uno\ntwo\nthree\nfour\nfive\n"})
        code, report = self.update()
        self.assertEqual(code, 1)
        self.assertIn("conflict", report)
        self.assertIn("<<<<<<< vendored", (self.root / "vendor/core.c").read_text())
        self.assertEqual(self.check()[0], 1)
        self.assertEqual(vendored.resolved(self.entries, self.registry, self.root), 0)
        self.assertEqual(vendored.load(self.registry)["vendor"]["taken_from"], new)
        self.assertEqual(self.check(), (0, ""))

    def test_added_and_deleted_upstream_files_need_a_decision(self):
        self.bump({"lib/extra.c": "new\n", "lib/core.h": None})
        code, report = self.update()
        self.assertEqual(code, 1)
        self.assertIn("upstream added lib/extra.c", report)
        self.assertIn("upstream deleted lib/core.h", report)


if __name__ == "__main__":
    unittest.main()
