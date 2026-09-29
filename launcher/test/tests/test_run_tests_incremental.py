"""run_tests.sh --build-only rebuilds exactly what a change touched.

    python -m unittest discover -s launcher/test/tests

Each test drives the real script into a scratch build directory and reads
which objects it compiled from its "  CC name.o" / "  SU name.o" lines. The
first build is cold (one compile per translation unit), so the class takes a
minute or two; the rest are incremental. ccache is off so every counted
compile is a real one.
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

TEST_DIR = Path(__file__).resolve().parents[1]
REPO = TEST_DIR.parents[1]
sys.path.insert(0, str(REPO / "scripts" / "device"))

import device  # noqa: E402

COMPILED = re.compile(r"^  (CC|SU) (\S+\.o)$", re.MULTILINE)


def read_depfile(path):
    """The prerequisites a gcc -MMD depfile lists for its object."""
    text = path.read_text().replace("\\\n", " ")
    _, _, deps = text.partition(": ")
    return [d.replace("\\ ", " ") for d in deps.split() if not d.endswith(":")]


class IncrementalBuildTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = Path(tempfile.mkdtemp(prefix="host-build-"))
        cls.env = dict(os.environ, TEST_BUILD_DIR=str(cls.build),
                       QUIET_INNER="1", AUTANA_CCACHE="0")
        cls.shell = device.git_bash()
        cls.cold = cls.run_build()
        cls.restore = []

    @classmethod
    def tearDownClass(cls):
        for path, times in cls.restore:
            os.utime(path, times)
        shutil.rmtree(cls.build, ignore_errors=True)

    @classmethod
    def run_build(cls, **extra_env):
        result = subprocess.run(
            [cls.shell, str(TEST_DIR / "run_tests.sh"), "--build-only"],
            env=dict(cls.env, **extra_env), capture_output=True, text=True)
        assert result.returncode == 0, result.stdout[-2000:] + result.stderr[-2000:]
        return COMPILED.findall(result.stdout)

    def touch(self, path):
        stat = path.stat()
        self.restore.append((path, (stat.st_atime, stat.st_mtime)))
        time.sleep(1.1)
        os.utime(path, None)

    def sources(self):
        out = subprocess.run([self.shell, str(TEST_DIR / "run_tests.sh"), "--print-sources"],
                             capture_output=True, text=True, check=True).stdout
        # Git Bash prints /c/... paths that native Python cannot open.
        return [REPO / "launcher" / line.strip().split("/launcher/", 1)[1]
                for line in out.splitlines() if line.strip()]

    def test_1_the_first_build_compiles_every_translation_unit(self):
        objects = {name for kind, name in self.cold if kind == "CC"}
        self.assertEqual(len(self.sources()) + 1, len(objects))
        self.assertTrue(any(kind == "SU" for kind, _ in self.cold))

    def test_2_a_rerun_with_no_change_compiles_nothing(self):
        self.assertEqual([], self.run_build())

    def test_3_touching_one_source_recompiles_one_object(self):
        source = next(p for p in self.sources()
                      if p.parent.name == "ui" and not p.name.startswith("suite_"))
        self.touch(source)
        rebuilt = self.run_build()
        self.assertEqual(1, len(rebuilt), rebuilt)
        self.assertEqual("CC", rebuilt[0][0])
        self.assertIn(source.stem, rebuilt[0][1])

    def test_4_touching_a_header_recompiles_exactly_its_includers(self):
        by_header = {}
        for sub in ("obj/plain", "su"):
            for dep in (self.build / sub).glob("*.d"):
                kind = "SU" if sub == "su" else "CC"
                for header in read_depfile(dep):
                    if header.endswith(".h") and "/launcher/main/" in header.replace("\\", "/"):
                        by_header.setdefault(Path(header), set()).add((kind, dep.stem + ".o"))
        total = len({name for _, name in self.cold})
        candidates = [(h, users) for h, users in by_header.items()
                      if 2 <= len({n for _, n in users}) < total // 2]
        self.assertTrue(candidates, "no header with a few includers to test with")
        header, expected = min(candidates, key=lambda c: str(c[0]))
        self.touch(header)
        self.assertEqual(sorted(expected), sorted(self.run_build()))

    def test_5_a_flags_change_rebuilds_everything(self):
        rebuilt = self.run_build(HOST_EXTRA_CFLAGS="-DHOST_BUILD_STAMP_TEST=1")
        self.assertEqual(sorted(self.cold), sorted(rebuilt))
        self.assertEqual([], self.run_build(HOST_EXTRA_CFLAGS="-DHOST_BUILD_STAMP_TEST=1"))


if __name__ == "__main__":
    unittest.main()
