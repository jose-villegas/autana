"""SUITE_READS (test/suites.h): the host runner skips and names the suites
that read a pack waiting on the bake lock, runs the rest, and fails a suite
that reads a pack it does not name. Built from test/suites.c and
test/pack_reads.c around fake suites (data/suite_reads_probe.c), linked with
the --wrap test/run_tests.sh gives host_tests."""

import os
import pathlib
import subprocess
import sys
import tempfile
import unittest

TOOLS = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))

from anim import track_host  # noqa: E402

LAUNCHER = TOOLS.parent
TEST = LAUNCHER / "test"
PROBE = TOOLS / "tests" / "data" / "suite_reads_probe.c"
SOURCES = (PROBE, PROBE.with_name("suite_reads_store.c"), TEST / "suites.c", TEST / "pack_reads.c")
SUITES = ("run_reads_built", "run_reads_its_own_folder", "run_reads_nothing", "run_reads_waiting")


class SuiteReadsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.folder = tempfile.TemporaryDirectory()
        cls.cc = track_host.compiler()
        cls.probe = cls.build("probe")
        cls.undeclared = cls.build("undeclared", "-DREAD_UNDECLARED")

    @classmethod
    def tearDownClass(cls):
        cls.folder.cleanup()

    @classmethod
    def build(cls, name, *defines):
        out = pathlib.Path(cls.folder.name) / f"{name}{track_host.EXE}"
        built = subprocess.run([cls.cc, *track_host.FLAGS, *defines, "-I", str(LAUNCHER / "main"), "-I", str(TEST),
                                *map(str, SOURCES), "-Wl,--wrap=asset_store_pack", "-o", str(out)],
                               capture_output=True, text=True)
        if built.returncode != 0:
            raise AssertionError(f"building {name} failed:\n{built.stderr}")
        return out

    def run_probe(self, program, waiting=""):
        env = {**os.environ, "AUTANA_PACKS_WAITING": waiting}
        return subprocess.run([str(program)], capture_output=True, text=True, env=env, timeout=30)

    def ran(self, run):
        return tuple(sorted(line.removeprefix("RAN ") for line in run.stdout.splitlines() if line.startswith("RAN ")))

    def test_a_tree_with_every_pack_built_runs_every_suite_and_reports_no_wait(self):
        run = self.run_probe(self.probe)
        self.assertEqual(run.returncode, 0, run.stdout)
        self.assertEqual(self.ran(run), SUITES)
        self.assertNotIn("waiting on lock", run.stdout)

    def test_a_waiting_pack_skips_only_the_suites_that_read_it_and_names_them(self):
        run = self.run_probe(self.probe, "probe_waiting,probe_unread")
        self.assertEqual(run.returncode, 0, run.stdout)
        self.assertEqual(self.ran(run), tuple(s for s in SUITES if s != "run_reads_waiting"))
        self.assertIn("waiting on lock: probe_waiting probe_unread (skipped suites: run_reads_waiting)\n", run.stdout)

    def test_a_waiting_pack_no_suite_reads_is_named_with_no_suite_skipped(self):
        run = self.run_probe(self.probe, "probe_unread")
        self.assertEqual(run.returncode, 0, run.stdout)
        self.assertEqual(self.ran(run), SUITES)
        self.assertIn("waiting on lock: probe_unread (skipped suites: none)\n", run.stdout)

    def test_a_suite_that_reads_a_pack_it_does_not_name_fails_the_run(self):
        run = self.run_probe(self.undeclared)
        self.assertNotEqual(run.returncode, 0, run.stdout)
        self.assertIn("FAIL: suite run_reads_undeclared read pack probe_built without naming it", run.stdout)
        self.assertEqual(run.stdout.count("FAIL:"), 1, run.stdout)


if __name__ == "__main__":
    unittest.main()
