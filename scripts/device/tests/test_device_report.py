import json
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import sys
DEVICE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(DEVICE))
import device_report


SAND_CAPTURE = """I (13050) screenshot: RUNSUITE run_sand_perf_suite
:3685:test_acid_bubbles_do_not_favour_one_wall:PASS
TEST_TIME name=test_acid_bubbles_do_not_favour_one_wall elapsed_ms=409
I (29661) device_tests: sand_step on 184x224 with 10304 grains: 7184 us
:295:test_a_full_size_step_fits_in_the_frame_budget:FAIL: Expected 7184 to be less than 5800. the simulation no longer fits in its share of the frame
TEST_TIME name=test_a_full_size_step_fits_in_the_frame_budget elapsed_ms=81
"""

PERF_TARGET_CAPTURE = """I (13050) screenshot: RUNSUITE run_sand_perf_suite
:295:test_a_full_size_step_fits_in_the_frame_budget:PASS
PERF TARGET test_a_full_size_step_fits_in_the_frame_budget: measured 7184 us, goal 5800 us, distance +23.9%
PERF TARGET SUMMARY: 1 unmet
"""


def write_index(directory, entries):
    index_path = Path(directory) / "index.jsonl"
    with open(index_path, "w", encoding="utf-8") as stream:
        for entry in entries:
            stream.write(json.dumps(entry) + "\n")
    return index_path


class ParseSuiteResultsTests(unittest.TestCase):
    def test_counts_pass_and_fail_and_captures_the_failure_message(self):
        passed, failed, failures = device_report.parse_suite_results(SAND_CAPTURE)
        self.assertEqual(passed, 1)
        self.assertEqual(failed, 1)
        self.assertEqual(failures, [(
            "test_a_full_size_step_fits_in_the_frame_budget",
            "Expected 7184 to be less than 5800. the simulation no longer fits in its "
            "share of the frame",
        )])

    def test_a_capture_with_no_result_lines_reports_zero_of_each(self):
        passed, failed, failures = device_report.parse_suite_results("nothing here\n")
        self.assertEqual((passed, failed, failures), (0, 0, []))


class ParsePerfTargetsTests(unittest.TestCase):
    def test_reads_targets_and_the_summary_line(self):
        targets, summary_unmet = device_report.parse_perf_targets(PERF_TARGET_CAPTURE)
        self.assertEqual(targets, [{
            "name": "test_a_full_size_step_fits_in_the_frame_budget",
            "measured": "7184",
            "goal": "5800",
            "distance": "+23.9%",
        }])
        self.assertEqual(summary_unmet, 1)

    def test_a_capture_with_no_perf_target_lines_reports_none_for_the_summary(self):
        targets, summary_unmet = device_report.parse_perf_targets(SAND_CAPTURE)
        self.assertEqual(targets, [])
        self.assertIsNone(summary_unmet)

    def test_reads_the_real_device_line_format(self):
        # Verbatim from a device capture: ESP-IDF log prefix, and a target name
        # with spaces. An anchored, no-space pattern parsed 0 of 36 such lines.
        text = ("I (32139) device_tests: PERF TARGET full-size step: measured 7159 us, "
                "goal 5800 us, distance +23%\n"
                "I (99001) device_tests: PERF TARGET SUMMARY: 17 unmet\n")
        targets, summary_unmet = device_report.parse_perf_targets(text)
        self.assertEqual([(t["name"], t["measured"], t["goal"]) for t in targets],
                         [("full-size step", "7159", "5800")])
        self.assertEqual(summary_unmet, 17)


REAL_PERF_RUN = """I (32138) device_tests: sand_step on 184x224 with 10304 grains: {step} us
I (32139) device_tests: PERF TARGET full-size step: measured {step} us, goal 5800 us, distance +23%
:3610:test_a_full_size_step_fits_in_the_frame_budget:PASS
I (35062) device_tests: portrait->landscape turn on a settled 248400-mass pool, 184x224: 7245 us per step, worst single step 15895 us
:3613:test_turning_a_settled_pool_fits:PASS
:223:test_the_grid_hash_stays_pegged:{hash}
"""


# Verbatim shape from 110900_runsuite-run_sand_perf_suite-run1_opus-present-clock-pin.log.
FRAME_TIME_BREAKDOWN_RUN = """I (359992) device_tests: frame time, falling sand checkerboard: sim 7167 us/frame
I (359993) device_tests: frame time, falling sand checkerboard: mark 950 us/frame
I (359994) device_tests: frame time, falling sand checkerboard: present 6181 us/frame
I (359994) device_tests: frame time, falling sand checkerboard: total 14298 us/frame
I (359995) device_tests: frame time, falling sand checkerboard: present is 43% of the total
:3870:test_a_real_frame_is_sim_plus_present_on_a_falling_sand_scene:PASS
I (364932) device_tests: frame time, lava stress: sim 92767 us/frame
I (364933) device_tests: frame time, lava stress: mark 1598 us/frame
I (364933) device_tests: frame time, lava stress: present 9480 us/frame
I (364934) device_tests: frame time, lava stress: total 103845 us/frame
I (364935) device_tests: present cost, lava stress scene, 184x224: mean 9480 us/frame over 20 frames (93 full-band, 34 gathered, 13 partial-band strip-sends)
:3871:test_present_cost_against_the_lava_stress_scene:PASS
"""


class ParseTimedResultsTests(unittest.TestCase):
    def test_takes_the_measurement_not_sizes_or_counts_earlier_on_the_line(self):
        rows = device_report.parse_timed_results(REAL_PERF_RUN.format(step=7159, hash="PASS"))
        self.assertEqual(rows["test_a_full_size_step_fits_in_the_frame_budget"][:2], ("PASS", 7159))
        self.assertEqual(rows["test_turning_a_settled_pool_fits"][:2], ("PASS", 7245))

    def test_a_frame_time_breakdown_is_not_the_headline_measurement(self):
        rows = device_report.parse_timed_results(FRAME_TIME_BREAKDOWN_RUN)
        self.assertEqual(rows["test_present_cost_against_the_lava_stress_scene"][:2], ("PASS", 9480))

    def test_a_test_logging_only_a_breakdown_is_measured_by_its_total(self):
        rows = device_report.parse_timed_results(FRAME_TIME_BREAKDOWN_RUN)
        self.assertEqual(rows["test_a_real_frame_is_sim_plus_present_on_a_falling_sand_scene"][:2],
                         ("PASS", 14298))

    def test_a_behaviour_test_carries_no_measurement(self):
        rows = device_report.parse_timed_results(REAL_PERF_RUN.format(step=7159, hash="PASS"))
        self.assertEqual(rows["test_the_grid_hash_stays_pegged"][:2], ("PASS", None))


class BatchSummaryTests(unittest.TestCase):
    def write_runs(self, directory, runs):
        entries = []
        for run, (step, hash_status) in enumerate(runs, 1):
            path = Path(directory) / f"run{run}.log"
            path.write_text(REAL_PERF_RUN.format(step=step, hash=hash_status), encoding="utf-8")
            entries.append({"suite": "run_sand_perf_suite", "run": run,
                            "capture": str(path), "error": None})
        return entries

    def summary(self, runs):
        with tempfile.TemporaryDirectory() as directory:
            entries = self.write_runs(directory, runs)
            return device_report.batch_summary_markdown(entries, {
                "build_id": "abc-diag", "owner": "t", "purpose": "p", "runs": len(runs),
                "worktree": "w", "commit": "c"})

    def test_reports_each_run_and_the_spread(self):
        text = self.summary([(7000, "PASS"), (7700, "PASS"), (7350, "PASS")])
        self.assertIn("| `test_a_full_size_step_fits_in_the_frame_budget` | 7000 | 7700 | 7350 "
                      "| 7000 | 7700 | 10.0% |", text)

    def test_a_result_that_changes_between_runs_of_one_image_is_listed(self):
        text = self.summary([(7167, "FAIL"), (10937, "PASS"), (7169, "FAIL")])
        section = text.split("### Result changed between runs")[1].split("###")[0]
        self.assertIn("| `test_the_grid_hash_stays_pegged` | FAIL | PASS | FAIL |", section)

    def test_stable_results_say_so(self):
        text = self.summary([(7000, "PASS"), (7010, "PASS")])
        self.assertIn("_None: every test gave the same result in every run._", text)

    def test_targets_are_tabulated_per_run(self):
        text = self.summary([(7159, "PASS"), (7169, "PASS")])
        self.assertIn("| full-size step | 7159 | 7169 | 5800 |", text)


class LooksLikePerfCaptureTests(unittest.TestCase):
    def test_a_device_tests_line_counts_as_perf_output(self):
        self.assertTrue(device_report.looks_like_perf_capture(SAND_CAPTURE))

    def test_a_perf_target_line_counts_as_perf_output(self):
        self.assertTrue(device_report.looks_like_perf_capture(PERF_TARGET_CAPTURE))

    def test_a_plain_capture_is_not_perf_output(self):
        self.assertFalse(device_report.looks_like_perf_capture(":1:test_one:PASS\n"))


class FindManifestEntryTests(unittest.TestCase):
    def test_returns_none_when_the_index_is_missing(self):
        with tempfile.TemporaryDirectory() as directory:
            missing = Path(directory) / "index.jsonl"
            self.assertIsNone(device_report.find_manifest_entry(
                Path(directory) / "capture.log", missing))

    def test_matches_the_capture_path_and_prefers_the_last_entry(self):
        with tempfile.TemporaryDirectory() as directory:
            capture = Path(directory) / "capture.log"
            capture.write_text("x")
            index_path = write_index(directory, [
                {"capture_path": str(capture), "suite": "first"},
                {"capture_path": str(Path(directory) / "other.log"), "suite": "unrelated"},
                {"capture_path": str(capture), "suite": "second"},
            ])
            entry = device_report.find_manifest_entry(capture, index_path)
            self.assertEqual(entry["suite"], "second")


class ReportPathForTests(unittest.TestCase):
    def test_a_plain_log_becomes_md_beside_it(self):
        self.assertEqual(device_report.report_path_for(Path("a/b/capture.log")),
                         Path("a/b/capture.md"))

    def test_a_gzipped_log_strips_both_suffixes(self):
        self.assertEqual(device_report.report_path_for(Path("a/b/capture.log.gz")),
                         Path("a/b/capture.md"))


class CountBudgetRowsTests(unittest.TestCase):
    def test_counts_rows_under_the_budget_header(self):
        markdown = "\n".join([
            "# Device Performance Report",
            "",
            device_report.BUDGET_TABLE_HEADER,
            "|---|---:|---:|---:|---|",
            "| `test_a` | 100 | 90 | +10.0% | PASS |",
            "| `test_b` | 100 | 110 | -10.0% | **FAIL** |",
            "",
            "> some trailing prose",
        ])
        self.assertEqual(device_report.count_budget_rows(markdown), 2)

    def test_zero_when_the_header_is_missing(self):
        self.assertEqual(device_report.count_budget_rows("no table here"), 0)

    def test_zero_when_the_table_has_no_rows(self):
        markdown = "\n".join([
            device_report.BUDGET_TABLE_HEADER,
            "|---|---:|---:|---:|---|",
            "",
            "> nothing matched",
        ])
        self.assertEqual(device_report.count_budget_rows(markdown), 0)


class DiscoverReportersTests(unittest.TestCase):
    def make_app(self, root, app, suite_file, registered_suite):
        app_dir = Path(root) / "launcher" / "main" / "apps" / app
        (app_dir / "tools").mkdir(parents=True)
        (app_dir / "tools" / "report_performance.py").write_text("# stub\n")
        (app_dir / suite_file).write_text(f"SUITE_REGISTER({registered_suite});\n")

    def test_zero_candidates_when_no_app_registers_the_suite(self):
        with tempfile.TemporaryDirectory() as directory:
            self.make_app(directory, "sand", "suite_sand_perf.c", "run_sand_perf_suite")
            matches = device_report.discover_reporters(directory, "run_cube_perf_suite")
            self.assertEqual(matches, [])

    def test_one_candidate_when_exactly_one_app_registers_the_suite(self):
        with tempfile.TemporaryDirectory() as directory:
            self.make_app(directory, "sand", "suite_sand_perf.c", "run_sand_perf_suite")
            self.make_app(directory, "cube", "suite_cube_perf.c", "run_cube_perf_suite")
            matches = device_report.discover_reporters(directory, "run_sand_perf_suite")
            self.assertEqual(len(matches), 1)
            self.assertEqual(matches[0]["app"], "sand")
            self.assertEqual(matches[0]["source"].name, "suite_sand_perf.c")

    def test_several_candidates_when_more_than_one_app_registers_the_same_suite(self):
        with tempfile.TemporaryDirectory() as directory:
            self.make_app(directory, "sand", "suite_sand_perf.c", "run_shared_suite")
            self.make_app(directory, "cube", "suite_cube_perf.c", "run_shared_suite")
            matches = device_report.discover_reporters(directory, "run_shared_suite")
            self.assertEqual(sorted(m["app"] for m in matches), ["cube", "sand"])

    def test_no_apps_directory_is_not_an_error(self):
        with tempfile.TemporaryDirectory() as directory:
            matches = device_report.discover_reporters(directory, "run_sand_perf_suite")
            self.assertEqual(matches, [])


class BuildBudgetSectionTests(unittest.TestCase):
    def test_no_worktree_states_the_reason(self):
        section = "\n".join(device_report.build_budget_section(None, "run_sand_perf_suite",
                                                                Path("capture.log"), ""))
        self.assertIn("no worktree recorded", section)

    def test_no_suite_states_the_reason(self):
        section = "\n".join(device_report.build_budget_section("C:/tree", None,
                                                                Path("capture.log"), ""))
        self.assertIn("no suite recorded", section)

    def test_a_worktree_that_does_not_exist_states_the_reason(self):
        section = "\n".join(device_report.build_budget_section(
            "C:/definitely/not/a/real/worktree-xyz", "run_sand_perf_suite",
            Path("capture.log"), ""))
        self.assertIn("does not exist", section)

    def test_zero_matching_reporters_states_the_reason(self):
        with tempfile.TemporaryDirectory() as directory:
            section = "\n".join(device_report.build_budget_section(
                directory, "run_sand_perf_suite", Path(directory) / "capture.log", ""))
        self.assertIn("no app's", section)

    def test_ambiguous_reporters_state_both_app_names(self):
        with tempfile.TemporaryDirectory() as directory:
            for app in ("sand", "cube"):
                app_dir = Path(directory) / "launcher" / "main" / "apps" / app
                (app_dir / "tools").mkdir(parents=True)
                (app_dir / "tools" / "report_performance.py").write_text("# stub\n")
                (app_dir / f"suite_{app}.c").write_text("SUITE_REGISTER(run_shared_suite);\n")
            section = "\n".join(device_report.build_budget_section(
                directory, "run_shared_suite", Path(directory) / "capture.log", ""))
        self.assertIn("ambiguous", section)
        self.assertIn("cube", section)
        self.assertIn("sand", section)

    def test_a_reporter_that_raises_is_reported_as_unavailable_not_fatal(self):
        with tempfile.TemporaryDirectory() as directory:
            app_dir = Path(directory) / "launcher" / "main" / "apps" / "sand"
            (app_dir / "tools").mkdir(parents=True)
            (app_dir / "tools" / "report_performance.py").write_text("# stub\n")
            (app_dir / "suite_sand_perf.c").write_text("SUITE_REGISTER(run_sand_perf_suite);\n")
            capture = Path(directory) / "capture.log"
            capture.write_text(SAND_CAPTURE)
            with mock.patch.object(device_report, "run_reporter",
                                   side_effect=RuntimeError("boom")):
                section = "\n".join(device_report.build_budget_section(
                    directory, "run_sand_perf_suite", capture, SAND_CAPTURE))
        self.assertIn("unavailable", section)
        self.assertIn("boom", section)

    def test_a_reporter_exiting_nonzero_is_reported_as_unavailable(self):
        with tempfile.TemporaryDirectory() as directory:
            app_dir = Path(directory) / "launcher" / "main" / "apps" / "sand"
            (app_dir / "tools").mkdir(parents=True)
            (app_dir / "tools" / "report_performance.py").write_text("# stub\n")
            (app_dir / "suite_sand_perf.c").write_text("SUITE_REGISTER(run_sand_perf_suite);\n")
            capture = Path(directory) / "capture.log"
            capture.write_text(SAND_CAPTURE)
            fake_result = mock.Mock(returncode=1, stdout="", stderr="source not found\n")
            with mock.patch.object(device_report, "run_reporter", return_value=fake_result):
                section = "\n".join(device_report.build_budget_section(
                    directory, "run_sand_perf_suite", capture, SAND_CAPTURE))
        self.assertIn("unavailable", section)
        self.assertIn("source not found", section)

    def test_a_table_with_rows_is_embedded_with_no_warning(self):
        with tempfile.TemporaryDirectory() as directory:
            app_dir = Path(directory) / "launcher" / "main" / "apps" / "sand"
            (app_dir / "tools").mkdir(parents=True)
            (app_dir / "tools" / "report_performance.py").write_text("# stub\n")
            (app_dir / "suite_sand_perf.c").write_text("SUITE_REGISTER(run_sand_perf_suite);\n")
            capture = Path(directory) / "capture.log"
            capture.write_text(SAND_CAPTURE)
            table = "\n".join([
                device_report.BUDGET_TABLE_HEADER,
                "|---|---:|---:|---:|---|",
                "| `test_a_full_size_step_fits_in_the_frame_budget` | 5800 | 7184 | "
                "-23.9% | **FAIL** |",
                "",
            ])

            def fake_run_reporter(reporter_path, capture_path, source_path, out_path,
                                  python_exe=None):
                Path(out_path).write_text(table, encoding="utf-8")
                return mock.Mock(returncode=0, stdout="", stderr="")

            with mock.patch.object(device_report, "run_reporter", side_effect=fake_run_reporter):
                section = "\n".join(device_report.build_budget_section(
                    directory, "run_sand_perf_suite", capture, SAND_CAPTURE))
        self.assertIn("test_a_full_size_step_fits_in_the_frame_budget", section)
        self.assertNotIn("WARNING", section)

    def test_a_zero_row_table_against_a_perf_capture_prints_a_loud_warning(self):
        """The incoming perf_guard/perf_target change makes report_performance.py
        find zero TEST_ASSERT_LESS_THAN_MESSAGE budgets - an empty table that must
        read as 'the reporter is out of step', not as 'nothing was measured'."""
        with tempfile.TemporaryDirectory() as directory:
            app_dir = Path(directory) / "launcher" / "main" / "apps" / "sand"
            (app_dir / "tools").mkdir(parents=True)
            (app_dir / "tools" / "report_performance.py").write_text("# stub\n")
            (app_dir / "suite_sand_perf.c").write_text("SUITE_REGISTER(run_sand_perf_suite);\n")
            capture = Path(directory) / "capture.log"
            capture.write_text(PERF_TARGET_CAPTURE)
            empty_table = "\n".join([
                "# Device Performance Report", "",
                device_report.BUDGET_TABLE_HEADER,
                "|---|---:|---:|---:|---|",
                "",
            ])

            def fake_run_reporter(reporter_path, capture_path, source_path, out_path,
                                  python_exe=None):
                Path(out_path).write_text(empty_table, encoding="utf-8")
                return mock.Mock(returncode=0, stdout="", stderr="")

            with mock.patch.object(device_report, "run_reporter", side_effect=fake_run_reporter):
                section = "\n".join(device_report.build_budget_section(
                    directory, "run_sand_perf_suite", capture, PERF_TARGET_CAPTURE))
        self.assertIn("WARNING", section)
        self.assertIn("zero rows", section)

    def test_a_zero_row_table_against_a_non_perf_capture_prints_no_warning(self):
        with tempfile.TemporaryDirectory() as directory:
            app_dir = Path(directory) / "launcher" / "main" / "apps" / "sand"
            (app_dir / "tools").mkdir(parents=True)
            (app_dir / "tools" / "report_performance.py").write_text("# stub\n")
            (app_dir / "suite_sand_perf.c").write_text("SUITE_REGISTER(run_sand_perf_suite);\n")
            capture = Path(directory) / "capture.log"
            plain = ":1:test_one:PASS\n"
            capture.write_text(plain)
            empty_table = "\n".join([device_report.BUDGET_TABLE_HEADER,
                                     "|---|---:|---:|---:|---|", ""])

            def fake_run_reporter(reporter_path, capture_path, source_path, out_path,
                                  python_exe=None):
                Path(out_path).write_text(empty_table, encoding="utf-8")
                return mock.Mock(returncode=0, stdout="", stderr="")

            with mock.patch.object(device_report, "run_reporter", side_effect=fake_run_reporter):
                section = "\n".join(device_report.build_budget_section(
                    directory, "run_sand_perf_suite", capture, plain))
        self.assertNotIn("WARNING", section)


class BuildReportMarkdownTests(unittest.TestCase):
    def test_a_synthetic_capture_with_pass_fail_and_perf_target_lines(self):
        with tempfile.TemporaryDirectory() as directory:
            capture = Path(directory) / "capture.log"
            capture.write_text(SAND_CAPTURE + PERF_TARGET_CAPTURE.splitlines()[-2] + "\n"
                               + PERF_TARGET_CAPTURE.splitlines()[-1] + "\n")
            index_path = write_index(directory, [{
                "capture_path": str(capture),
                "suite": "run_sand_perf_suite",
                "build_id": "deadbeef-diag",
                "worktree": None,
                "commit": "cafef00d",
                "owner": "agent",
                "purpose": "nightly perf",
                "reason": "timeout",
                "error": None,
            }])
            markdown = device_report.build_report_markdown(capture, index_path)

        self.assertIn("# Device Capture Report", markdown)
        self.assertIn("Suite: `run_sand_perf_suite`", markdown)
        self.assertIn("Build: `deadbeef-diag`", markdown)
        self.assertIn("Commit: `cafef00d` (worktree unknown)", markdown)
        self.assertIn("PASS: 1  FAIL: 1", markdown)
        self.assertIn("### Failures", markdown)
        self.assertIn("test_a_full_size_step_fits_in_the_frame_budget", markdown)
        self.assertIn("## Performance Targets", markdown)
        self.assertIn("PERF TARGET SUMMARY: 1 unmet", markdown)
        self.assertIn("no worktree recorded", markdown)

    def test_a_capture_with_no_manifest_entry_says_so(self):
        with tempfile.TemporaryDirectory() as directory:
            capture = Path(directory) / "capture.log"
            capture.write_text(SAND_CAPTURE)
            index_path = Path(directory) / "index.jsonl"
            markdown = device_report.build_report_markdown(capture, index_path)
        self.assertIn("no matching entry found", markdown)

    def test_a_capture_with_no_result_lines_says_so_instead_of_zero_zero(self):
        with tempfile.TemporaryDirectory() as directory:
            capture = Path(directory) / "capture.log"
            capture.write_text("nothing interesting\n")
            index_path = Path(directory) / "index.jsonl"
            markdown = device_report.build_report_markdown(capture, index_path)
        self.assertIn("No `PASS`/`FAIL` result lines found", markdown)

    def test_a_gzipped_capture_is_read_transparently(self):
        import gzip
        with tempfile.TemporaryDirectory() as directory:
            capture = Path(directory) / "capture.log.gz"
            with gzip.open(capture, "wt", encoding="utf-8") as stream:
                stream.write(SAND_CAPTURE)
            index_path = Path(directory) / "index.jsonl"
            markdown = device_report.build_report_markdown(capture, index_path)
        self.assertIn("PASS: 1  FAIL: 1", markdown)


class WriteReportForCaptureTests(unittest.TestCase):
    def test_writes_the_md_file_beside_the_capture_and_returns_its_path(self):
        with tempfile.TemporaryDirectory() as directory:
            capture = Path(directory) / "capture.log"
            capture.write_text(SAND_CAPTURE)
            index_path = Path(directory) / "index.jsonl"
            out_path = device_report.write_report_for_capture(capture, index_path)
            self.assertEqual(out_path, capture.with_name("capture.md"))
            self.assertIn("# Device Capture Report", out_path.read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
