"""The complexity gate's empty-scan guard, driven through main() with
clang-tidy and the compile database stubbed out."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

QUALITY = Path(__file__).resolve().parents[3] / "launcher/tools/quality"
sys.path.insert(0, str(QUALITY))

import complexity_gate  # noqa: E402

DATA_ONLY = """\
/* GENERATED FILE - do not edit. */
#include <stdint.h>
const uint16_t mesh_indices[] = { 0, 1, 2, 2, 3, 0 };
const struct { int a; int b; } mesh_bounds = { 1, 2 };
"""

WITH_FUNCTION = """\
int pick(int a)
{
    if (a > 2) {
        return 1;
    }
    return 0;
}
"""


class EmptyScanGuardTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)

    def source(self, name, text):
        path = Path(self.tmp.name) / name
        path.write_text(text, encoding="utf-8")
        return str(path)

    def run_gate(self, files, changed):
        """main() over `files` with a clang-tidy that reports nothing."""
        silent = subprocess.CompletedProcess([], 0, "", "")
        argv = ["complexity_gate.py",
                "--baseline", str(Path(self.tmp.name) / "baseline.txt")]
        if changed:
            argv += ["--changed", "origin/main"]
        patches = [
            mock.patch.object(sys, "argv", argv),
            mock.patch.object(complexity_gate, "resolve_clang_tidy",
                              return_value=("clang-tidy", "19")),
            mock.patch.object(complexity_gate, "build_compile_db",
                              return_value=(Path("db.json"), files,
                                            {"idf": 0, "host": 0, "tools": 0})),
            mock.patch.object(complexity_gate, "check_main_coverage",
                              return_value=(len(files), [])),
            mock.patch.object(complexity_gate, "changed_files",
                              return_value=files),
            mock.patch.object(complexity_gate, "changed_rel_paths",
                              return_value=set()),
            mock.patch.object(complexity_gate, "scan_vendored",
                              return_value={}),
            mock.patch.object(complexity_gate, "run_clang_tidy_parallel",
                              return_value=silent),
        ]
        for p in patches:
            p.start()
            self.addCleanup(p.stop)
        return complexity_gate.main()

    def test_changed_data_only_file_is_not_a_broken_scan(self):
        files = [self.source("mesh_generated.c", DATA_ONLY)]
        self.assertEqual(self.run_gate(files, changed=True), 0)

    def test_changed_file_with_a_function_still_needs_a_score(self):
        files = [self.source("mesh_generated.c", DATA_ONLY),
                 self.source("pick.c", WITH_FUNCTION)]
        with self.assertRaisesRegex(SystemExit, "ZERO functions"):
            self.run_gate(files, changed=True)

    def test_full_scan_of_data_only_files_is_still_a_broken_scan(self):
        files = [self.source("mesh_generated.c", DATA_ONLY)]
        with self.assertRaisesRegex(SystemExit, "ZERO functions"):
            self.run_gate(files, changed=False)


if __name__ == "__main__":
    unittest.main()
