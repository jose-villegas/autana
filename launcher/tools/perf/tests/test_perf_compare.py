"""Regression tests for the revision performance report comparison.

    python -m unittest discover -s launcher/tools/perf/tests
"""
import os
import pathlib
import stat
import subprocess
import sys
import tempfile
import unittest


PERF = pathlib.Path(__file__).resolve().parents[1]
FIXTURES = pathlib.Path(__file__).resolve().parent / "fixtures"
sys.path.insert(0, str(PERF))

import perf_compare  # noqa: E402


class PerfCompareTest(unittest.TestCase):
    def sponza_runs(self, side):
        return [perf_compare.parse_report(path)
                for path in sorted((FIXTURES / "sponza_runs").glob(f"{side}_*.txt"))]

    def make_revision_tree(self, directory, name):
        tree = pathlib.Path(directory) / name
        tree.mkdir()
        (tree / "firmware.txt").write_text(name, encoding="utf-8")
        subprocess.run(["git", "init", "-q", str(tree)], check=True)
        subprocess.run(["git", "-C", str(tree), "add", "firmware.txt"], check=True)
        subprocess.run(["git", "-C", str(tree), "-c", "user.name=test",
                        "-c", "user.email=test@example.invalid", "commit", "-qm", name], check=True)
        return tree

    def write_compare_fixtures(self, directory):
        root = pathlib.Path(directory)
        fake_bin = root / "bin"
        fake_bin.mkdir()
        state = root / "project"
        calls = root / "calls"
        autana_calls = root / "autana-calls"
        autana = fake_bin / "autana"
        autana.write_text(
            "#!/bin/sh\n"
            "wait=\n"
            "if [ \"$1\" = --wait ]; then wait=$2; shift 2; fi\n"
            "printf '%s %s\\n' \"$wait\" \"$1\" >> \"$PERF_TEST_AUTANA_CALLS\"\n"
            "case \"$1\" in\n"
            "  status) echo unlocked ;;\n"
            "  buildid) if [ -n \"${PERF_TEST_BUSY_ALWAYS:-}\" ] || { [ -n \"${PERF_TEST_BUSY:-}\" ] && [ -z \"$wait\" ]; }; then\n"
            "      echo 'the board is busy - held by device_report-performance:75040'\n"
            "    else sed 's/^/BUILD_ID=/' \"$(cat \"$PERF_TEST_PROJECT\")/launcher/build.diag/build_id.txt\"; fi ;;\n"
            "  *) exit 9 ;;\n"
            "esac\n", encoding="utf-8")
        report = root / "report.sh"
        report.write_text(
            "#!/bin/sh\n"
            "set -eu\n"
            "project=\n"
            "out=\n"
            "while [ $# -gt 0 ]; do\n"
            "  case \"$1\" in\n"
            "    --project) project=$2; shift 2 ;;\n"
            "    *.md) out=$1; shift ;;\n"
            "    *) shift ;;\n"
            "  esac\n"
            "done\n"
            "[ -n \"$project\" ]\n"
            "printf '%s\\n' \"$project\" > \"$PERF_TEST_PROJECT\"\n"
            "mkdir -p \"$project/launcher/build.diag\"\n"
            "printf '%s-diag\\n' \"$(git -C \"$project\" hash-object firmware.txt | cut -c 1-12)\" > \"$project/launcher/build.diag/build_id.txt\"\n"
            "printf '%s:%s\\n' \"$PWD\" \"$project\" >> \"$PERF_TEST_CALLS\"\n"
            "count=0\n"
            "[ -f \"$PERF_TEST_COUNT\" ] && count=$(cat \"$PERF_TEST_COUNT\")\n"
            "count=$((count + 1))\n"
            "printf '%s\\n' \"$count\" > \"$PERF_TEST_COUNT\"\n"
            "case \"${PERF_TEST_MODE:-ok}\" in\n"
            "  fail-once) [ \"$count\" -eq 1 ] && exit 7 ;;\n"
            "  cut-short) printf '# incomplete capture\\n' > \"$out\"; exit 1 ;;\n"
            "  timeout) sleep 2 ;;\n"
            "esac\n"
            "read -r ignored || true\n"
            "printf '| Test | Measured (us) |\\n|---|---:|\\n| `row` | 10 |\\n' > \"$out\"\n"
            "[ \"${PERF_TEST_MODE:-ok}\" = budget-fail ] && exit 1\n",
            encoding="utf-8")
        for path in (autana, report):
            path.chmod(path.stat().st_mode | stat.S_IXUSR)
        return fake_bin, report, state, calls, autana_calls, root / "count"

    def run_compare_fixture(self, directory, mode="ok", runs=1, timeout=5):
        tree_a = self.make_revision_tree(directory, "before")
        tree_b = self.make_revision_tree(directory, "after")
        fake_bin, report, state, calls, autana_calls, count = self.write_compare_fixtures(directory)
        out = pathlib.Path(directory) / "out"
        env = dict(os.environ, PATH=f"{fake_bin}{os.pathsep}{os.environ['PATH']}",
                   PERF_TEST_PROJECT=str(state), PERF_TEST_CALLS=str(calls),
                   PERF_TEST_COUNT=str(count), PERF_TEST_AUTANA_CALLS=str(autana_calls), PERF_TEST_MODE=mode)
        done = subprocess.run(
            ["sh", str(PERF / "perf_compare.sh"), "-o", str(out), "--no-restore", "--runs", str(runs),
             "--timeout", str(timeout), str(tree_a), str(tree_b), "--", "sh", str(report)],
            cwd=PERF.parent, env=env, capture_output=True, text=True, timeout=15)
        return done, tree_a, tree_b, out, calls, count

    def write_capture_command(self, directory):
        """Stands in for `autana suite --out PATH`: the raw console lands at
        PATH, then a summary report with no timings lands beside it."""
        root = pathlib.Path(directory)
        command = root / "capture.sh"
        command.write_text(
            "#!/bin/sh\n"
            "set -eu\n"
            "project=\n"
            "out=\n"
            "while [ $# -gt 0 ]; do\n"
            "  case \"$1\" in\n"
            "    --project) project=$2; shift 2 ;;\n"
            "    --out) out=$2; shift 2 ;;\n"
            "    *) shift ;;\n"
            "  esac\n"
            "done\n"
            "[ -n \"$project\" ] && [ -n \"$out\" ]\n"
            "printf '%s\n' \"$project\" > \"$PERF_TEST_PROJECT\"\n"
            "mkdir -p \"$project/launcher/build.diag\"\n"
            "build=$(git -C \"$project\" hash-object firmware.txt | cut -c 1-12)-diag\n"
            "printf '%s\n' \"$build\" > \"$project/launcher/build.diag/build_id.txt\"\n"
            "[ -n \"${PERF_TEST_NO_BOOT_BUILD_ID:-}\" ] || echo \"booted BUILD_ID=$build to its console\"\n"
            "cp \"$PERF_TEST_FIXTURE\" \"$out\"\n"
            "printf '# Device Capture Report\n\nPASS: 2  FAIL: 0\n' > \"${out%.*}.md\"\n",
            encoding="utf-8")
        command.chmod(command.stat().st_mode | stat.S_IXUSR)
        return command

    def test_a_capture_path_is_given_apart_from_the_report_and_its_timings_are_compared(self):
        with tempfile.TemporaryDirectory() as directory:
            tree_a = self.make_revision_tree(directory, "before")
            tree_b = self.make_revision_tree(directory, "after")
            fake_bin, _, state, calls, _, count = self.write_compare_fixtures(directory)
            command = self.write_capture_command(directory)
            out = pathlib.Path(directory) / "out"
            env = dict(os.environ, PATH=f"{fake_bin}{os.pathsep}{os.environ['PATH']}",
                       PERF_TEST_PROJECT=str(state), PERF_TEST_AUTANA_CALLS=str(pathlib.Path(directory) / "autana-calls"),
                       PERF_TEST_FIXTURE=str(FIXTURES / "sponza_capture.txt"))
            done = subprocess.run(
                ["sh", str(PERF / "perf_compare.sh"), "-o", str(out), "--no-restore", "--runs", "2",
                 str(tree_a), str(tree_b), "--", "sh", str(command), "--out", "@CAPTURE@"],
                cwd=PERF.parent, env=env, capture_output=True, text=True, timeout=30)
            summary = (out / "summary.md").read_text(encoding="utf-8") if (out / "summary.md").is_file() else ""
            capture = (out / "a" / "run_1.capture.log").read_text(encoding="utf-8")
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertIn("sponza both cores: mean 51000us", capture)
        self.assertIn("| `fitted-full` | 38000 | 38000 | 38000 | 38000 | +0 | no change |", summary)
        self.assertIn("| `sponza` | 51000 | 51000 | 51000 | 51000 | +0 | no change |", summary)

    def test_the_build_id_comes_from_the_boot_the_capture_logged_under_its_lock(self):
        with tempfile.TemporaryDirectory() as directory:
            tree_a = self.make_revision_tree(directory, "before")
            tree_b = self.make_revision_tree(directory, "after")
            fake_bin, _, state, _, autana_calls, _ = self.write_compare_fixtures(directory)
            command = self.write_capture_command(directory)
            out = pathlib.Path(directory) / "out"
            env = dict(os.environ, PATH=f"{fake_bin}{os.pathsep}{os.environ['PATH']}",
                       PERF_TEST_PROJECT=str(state), PERF_TEST_AUTANA_CALLS=str(autana_calls),
                       PERF_TEST_BUSY_ALWAYS="1", PERF_TEST_FIXTURE=str(FIXTURES / "sponza_capture.txt"))
            done = subprocess.run(
                ["sh", str(PERF / "perf_compare.sh"), "-o", str(out), "--no-restore", "--runs", "2", "--timeout", "5",
                 str(tree_a), str(tree_b), "--", "sh", str(command), "--out", "@CAPTURE@"],
                cwd=PERF.parent, env=env, capture_output=True, text=True, timeout=30)
            expected = (tree_a / "launcher" / "build.diag" / "build_id.txt").read_text(encoding="utf-8").strip()
            status = (out / "a" / "run_1.status").read_text(encoding="utf-8").strip()
            recorded_calls = autana_calls.read_text(encoding="utf-8").splitlines()
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertEqual(status, f"captured {expected}")
        self.assertFalse(any("buildid" in call for call in recorded_calls))

    def test_busy_buildid_refusal_is_avoided_with_the_capture_wait(self):
        with tempfile.TemporaryDirectory() as directory:
            tree_a = self.make_revision_tree(directory, "before")
            tree_b = self.make_revision_tree(directory, "after")
            fake_bin, _, state, _, autana_calls, _ = self.write_compare_fixtures(directory)
            command = self.write_capture_command(directory)
            out = pathlib.Path(directory) / "out"
            env = dict(os.environ, PATH=f"{fake_bin}{os.pathsep}{os.environ['PATH']}",
                       PERF_TEST_PROJECT=str(state), PERF_TEST_AUTANA_CALLS=str(autana_calls), PERF_TEST_BUSY="1",
                       PERF_TEST_NO_BOOT_BUILD_ID="1",
                       PERF_TEST_FIXTURE=str(FIXTURES / "sponza_capture.txt"))
            done = subprocess.run(
                ["sh", str(PERF / "perf_compare.sh"), "-o", str(out), "--no-restore", "--runs", "2", "--timeout", "5",
                 str(tree_a), str(tree_b), "--", "sh", str(command), "--out", "@CAPTURE@"],
                cwd=PERF.parent, env=env, capture_output=True, text=True, timeout=30)
            expected = (tree_a / "launcher" / "build.diag" / "build_id.txt").read_text(encoding="utf-8").strip()
            status = (out / "a" / "run_1.status").read_text(encoding="utf-8").strip()
            recorded_calls = autana_calls.read_text(encoding="utf-8").splitlines()
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertEqual(status, f"captured {expected}")
        self.assertTrue(all(call == " status" or call.startswith("5 ") for call in recorded_calls))
        self.assertIn("5 buildid", recorded_calls)

    def test_busy_buildid_without_a_boot_log_fails_without_recording_the_refusal_as_an_id(self):
        with tempfile.TemporaryDirectory() as directory:
            tree_a = self.make_revision_tree(directory, "before")
            tree_b = self.make_revision_tree(directory, "after")
            fake_bin, _, state, _, autana_calls, _ = self.write_compare_fixtures(directory)
            command = self.write_capture_command(directory)
            out = pathlib.Path(directory) / "out"
            env = dict(os.environ, PATH=f"{fake_bin}{os.pathsep}{os.environ['PATH']}",
                       PERF_TEST_PROJECT=str(state), PERF_TEST_AUTANA_CALLS=str(autana_calls),
                       PERF_TEST_BUSY_ALWAYS="1", PERF_TEST_NO_BOOT_BUILD_ID="1",
                       PERF_TEST_FIXTURE=str(FIXTURES / "sponza_capture.txt"))
            done = subprocess.run(
                ["sh", str(PERF / "perf_compare.sh"), "-o", str(out), "--no-restore", "--runs", "2", "--timeout", "5",
                 str(tree_a), str(tree_b), "--", "sh", str(command), "--out", "@CAPTURE@"],
                cwd=PERF.parent, env=env, capture_output=True, text=True, timeout=30)
            status = (out / "a" / "run_1.status").read_text(encoding="utf-8").strip()
        self.assertNotEqual(done.returncode, 0)
        self.assertNotIn("the board is busy", status)
        self.assertTrue(status.startswith("build id "))

    def test_real_suite_lines_give_one_row_per_variant(self):
        rows = perf_compare.parse_report(FIXTURES / "sponza_capture.txt")
        self.assertEqual(rows, {"sponza": 51000, "lite": 42000, "flat": 39000,
                                "fitted": 41000, "fitted-full": 38000})

    def shell_path(self, path):
        if os.name != "nt":
            return str(path)
        return subprocess.check_output(["cygpath", "-u", str(path)], text=True).strip()

    def test_shell_uses_current_reporter_with_project_and_closed_stdin(self):
        with tempfile.TemporaryDirectory() as directory:
            done, tree_a, tree_b, out, calls, _ = self.run_compare_fixture(directory)
            recorded = calls.read_text(encoding="utf-8").splitlines()
            summary_exists = (out / "summary.md").is_file()
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertEqual(recorded, [
            f"{self.shell_path(PERF.parent)}:{self.shell_path(tree_a)}",
            f"{self.shell_path(PERF.parent)}:{self.shell_path(tree_b)}",
        ])
        self.assertTrue(summary_exists)

    def test_shell_continues_after_one_failed_capture(self):
        with tempfile.TemporaryDirectory() as directory:
            done, _, _, _, _, count = self.run_compare_fixture(directory, mode="fail-once", runs=2)
            calls = int(count.read_text(encoding="utf-8"))
        self.assertNotEqual(done.returncode, 0)
        self.assertEqual(calls, 4)
        self.assertIn("capture failed", done.stderr)

    def test_shell_keeps_a_budget_failure_but_rejects_a_cut_short_capture(self):
        with tempfile.TemporaryDirectory() as directory:
            done, _, _, out, _, count = self.run_compare_fixture(
                directory, mode="budget-fail", runs=1)
            calls = int(count.read_text(encoding="utf-8"))
            reports = (out / "a" / "reports.list").read_text(encoding="utf-8").splitlines()
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertEqual(calls, 2)
        self.assertEqual(len(reports), 1)

        with tempfile.TemporaryDirectory() as directory:
            done, _, _, out, _, count = self.run_compare_fixture(
                directory, mode="cut-short", runs=2)
            calls = int(count.read_text(encoding="utf-8"))
            summary_exists = (out / "summary.md").exists()
        self.assertNotEqual(done.returncode, 0)
        self.assertEqual(calls, 2)
        self.assertFalse(summary_exists)

    def test_shell_stops_after_two_capture_timeouts(self):
        with tempfile.TemporaryDirectory() as directory:
            done, _, _, _, _, count = self.run_compare_fixture(directory, mode="timeout", runs=3, timeout=1)
            calls = int(count.read_text(encoding="utf-8"))
        self.assertNotEqual(done.returncode, 0)
        self.assertEqual(calls, 2)
        self.assertIn("two consecutive capture failures", done.stderr)

    def test_no_script_pauses_for_enter_without_a_terminal(self):
        tools = PERF.parent
        for script in (tools / "build" / "build.sh", tools / "device" / "device_report.sh"):
            lines = script.read_text(encoding="utf-8").splitlines()
            for number, line in enumerate(lines):
                if "read -r" in line and ("Press Enter" in line or "dismissed" in line):
                    window = " ".join(lines[max(0, number - 3):number + 1])
                    self.assertIn("[ -t 0 ]", window, f"{script.name}:{number + 1} reads without a terminal check")

    def test_a_markdown_table_uses_its_measured_column(self):
        rows = perf_compare.parse_report(FIXTURES / "sand_a.md")
        self.assertEqual(rows["hot"], 2000)

    def test_sponza_mean_lines_are_generic_named_number_rows(self):
        rows = perf_compare.parse_report(FIXTURES / "sponza.txt")
        self.assertEqual(rows, {"sponza": 51000, "lite": 42000, "flat": 39000})

    def test_worst_of_each_side_is_compared_per_row(self):
        a = [{"full": 100, "lite": 80}, {"full": 110, "lite": 78}]
        b = [{"full": 105, "lite": 82}, {"full": 104, "lite": 88}]
        self.assertEqual(perf_compare.compare(a, b), [
            ("full", 110, 105, -5, "improved"),
            ("lite", 80, 88, 8, "regressed"),
        ])

    def test_controls_turn_a_small_delta_into_no_change(self):
        a = [perf_compare.parse_report(FIXTURES / "sand_a.md")]
        b = [perf_compare.parse_report(FIXTURES / "sand_b.md")]
        controls = ("test_a_full_size_step_fits_in_the_frame_budget",
                    "test_flipping_gravity_on_a_settled_pile_fits_in_the_frame_budget")
        hot = next(row for row in perf_compare.compare(a, b, controls) if row[0] == "hot")
        self.assertEqual(hot,
                         ("hot", 2000, 2010, 10, "no change"))

    def test_a_coherent_shifted_run_is_flagged_with_its_size(self):
        a_paths = sorted((FIXTURES / "sponza_runs").glob("a_*.txt"))
        b_paths = sorted((FIXTURES / "sponza_runs").glob("b_*.txt"))
        shifts = perf_compare.whole_run_shifts(
            [perf_compare.parse_report(path) for path in b_paths])
        self.assertEqual(len(shifts), 1)
        self.assertEqual(shifts[0][0], 3)
        self.assertAlmostEqual(shifts[0][1], 0.4, places=1)
        with tempfile.TemporaryDirectory() as directory:
            summary_path = pathlib.Path(directory) / "summary.md"
            perf_compare.write_summary(
                summary_path, "before", "after", ["a"] * 3, ["b"] * 3,
                a_paths, b_paths)
            summary = summary_path.read_text(encoding="utf-8")
        self.assertIn("B run 3: +0.4%", summary)
        self.assertIn("5 of 5 rows aligned", summary)

    def test_normal_run_scatter_is_not_flagged(self):
        self.assertEqual(perf_compare.whole_run_shifts(self.sponza_runs("a")), [])

    def test_a_regression_present_in_every_run_is_not_a_whole_run_shift(self):
        a = [{"one": 100 + offset, "two": 200 + offset, "three": 300 + offset}
             for offset in (-1, 0, 1)]
        b = [{name: round(value * 1.1) for name, value in run.items()} for run in a]
        self.assertEqual(perf_compare.whole_run_shifts(b), [])
        self.assertTrue(all(row[4] == "regressed" for row in perf_compare.compare(a, b)))

    def test_summary_lists_worst_values_and_build_ids(self):
        with tempfile.TemporaryDirectory() as directory:
            out = pathlib.Path(directory) / "summary.md"
            perf_compare.write_summary(
                out, "before", "after", ["before-diag"], ["after-diag"],
                [FIXTURES / "sand_a.md"], [FIXTURES / "sand_b.md"])
            summary = out.read_text(encoding="utf-8")
            aggregate_path = out.parent / "worst.md"
            perf_compare.write_aggregate(
                aggregate_path, [perf_compare.parse_report(FIXTURES / "sand_a.md")])
            aggregate = aggregate_path.read_text(encoding="utf-8")
        self.assertIn("`before-diag`", summary)
        self.assertIn("| `hot` | 2000 | 2000 | 2010 | 2010 | +10 | no change |", summary)
        self.assertIn("| `hot` | ? | 2000 | ? | measured |", aggregate)

if __name__ == "__main__":
    unittest.main()
