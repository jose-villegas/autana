"""What a filtered RUNSUITE prints, read by the tooling that parses it.

    python -m unittest discover -s launcher/test/tests

The requests run through the real runner (host_runner.py), so the tests
below fail when the firmware's wording drifts from what device.py expects.
"""
import re
import subprocess
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import host_runner  # noqa: E402
import port_guard  # noqa: E402,F401  (before device: no test reaches a real board)
import device  # noqa: E402
import device_report  # noqa: E402
import device_capture  # noqa: E402

HEADER = (host_runner.TEST_DIR / "suites.h").read_text(encoding="utf-8")
FILTER_MAX = int(re.search(r"#define SUITE_FILTER_MAX (\d+)", HEADER).group(1))
FILTER_LEN = int(re.search(r"#define SUITE_FILTER_LEN (\d+)", HEADER).group(1))
FIXTURE = "filter_fixture_suite"
CASES = "case_fixture_suite"
NAME_MAX = int(re.search(r"#define SUITE_TEST_NAME_MAX\s+(\d+)", HEADER).group(1))


def probes(output):
    return re.findall(r"^PROBE_RAN (\S+)$", output, re.MULTILINE)


class FilteredRunOutputTests(unittest.TestCase):
    def test_a_selected_test_runs_once_and_an_unselected_one_not_at_all(self):
        ran = probes(host_runner.run(f"{FIXTURE} fire"))
        self.assertEqual(ran, ["test_fire_fits"])

    def test_the_completion_line_is_what_the_host_parses(self):
        output = host_runner.run(f"{FIXTURE} fire")
        complete = device_report.SUITE_COMPLETE_RE.search(output)
        self.assertEqual((complete.group("found"), complete.group("selected"),
                          complete.group("unmatched")), ("1", "1", "0"))
        device.check_test_filter(output.encode(), FIXTURE, ["x"])

    def test_every_test_is_listed_with_whether_it_was_selected(self):
        output = host_runner.run(f"{FIXTURE} fire")
        self.assertEqual(re.findall(r"^SUITE_TEST name=(\w+) selected=([01])$", output, re.MULTILINE),
                         [("test_fire_fits", "1"), ("test_gas_fits", "0"), ("test_water_fits", "0")])

    def test_a_pattern_matching_nothing_runs_nothing_and_lists_every_name(self):
        output = host_runner.run(f"{FIXTURE} fyre")
        self.assertEqual(probes(output), [])
        with self.assertRaises(device.NoTestMatched) as caught:
            device.check_test_filter(output.encode(), FIXTURE, ["fyre"])
        self.assertIn("--test fyre matches no test", str(caught.exception))
        for name in ("test_fire_fits", "test_gas_fits", "test_water_fits"):
            self.assertIn(name, str(caught.exception))

    def test_a_miss_beside_a_hit_runs_the_hit_then_fails(self):
        output = host_runner.run(f"{FIXTURE} fire,fyre")
        self.assertEqual(probes(output), ["test_fire_fits"])
        with self.assertRaises(device.NoTestMatched) as caught:
            device.check_test_filter(output.encode(), FIXTURE, ["fire", "fyre"])
        self.assertIn("--test fyre matches", str(caught.exception))

    def test_a_plain_request_after_a_filtered_one_runs_every_test(self):
        output = host_runner.run(f"{FIXTURE} fire", FIXTURE)
        self.assertEqual(probes(output), ["test_fire_fits", "test_fire_fits", "test_gas_fits",
                                          "test_water_fits"])

    def test_limits_are_accepted_at_the_limit_and_refused_one_past(self):
        def outcome(request):
            output = host_runner.run(request)
            try:
                device.check_test_filter(output.encode(), FIXTURE, ["x"])
            except device.NoTestMatched:
                return "accepted"
            except device.TestFilterError:
                return "refused"
            return "accepted"

        longest = "x" * (FILTER_LEN - 1)
        self.assertEqual(outcome(f"{FIXTURE} {longest}"), "accepted")
        self.assertEqual(outcome(f"{FIXTURE} {longest}x"), "refused")
        self.assertEqual(outcome(f"{FIXTURE} " + ",".join(["fire"] * FILTER_MAX)), "accepted")
        self.assertEqual(outcome(f"{FIXTURE} " + ",".join(["fire"] * (FILTER_MAX + 1))), "refused")


class TestCaseOutputTests(unittest.TestCase):
    def test_each_case_is_named_by_its_segments_and_left_out_ones_are_skipped(self):
        self.assertEqual(probes(host_runner.run(CASES)), [
            "test_case/sponza-fitted/portrait", "test_case/sponza-fitted/landscape",
            "test_case/sponza-fitted-full/portrait", "test_case/sponza-fitted-full/landscape",
            "test_case/capybara/front/portrait"])

    def test_a_scene_pattern_picks_its_own_cases_and_not_a_longer_scene(self):
        self.assertEqual(probes(host_runner.run(f"{CASES} sponza-fitted")),
                         ["test_case/sponza-fitted/portrait", "test_case/sponza-fitted/landscape"])

    def test_a_group_pattern_picks_one_take(self):
        self.assertEqual(probes(host_runner.run(f"{CASES} capybara/front,sponza-fitted-full/landscape")),
                         ["test_case/sponza-fitted-full/landscape", "test_case/capybara/front/portrait"])

    def test_every_case_is_listed_by_its_full_name(self):
        output = host_runner.run(f"{CASES} capybara")
        self.assertIn("SUITE_TEST name=test_case/capybara/front/portrait selected=1", output)
        self.assertIn("SUITE_TEST name=test_case/sponza-fitted/portrait selected=0", output)

    def test_a_part_of_a_segment_picks_nothing_and_is_counted_unmatched(self):
        output = host_runner.run(f"{CASES} sponza-fitt,capybara")
        self.assertEqual(probes(output), ["test_case/capybara/front/portrait"])
        self.assertEqual(device_report.SUITE_COMPLETE_RE.search(output).group("unmatched"), "1")

    def test_a_name_past_the_limit_or_of_other_characters_is_an_overflow_that_fails_the_run(self):
        result = subprocess.run([str(host_runner.binary()), "--run", "case_limit_suite"],
                                capture_output=True, text=True)
        self.assertEqual([len(name) for name in probes(result.stdout)], [NAME_MAX])
        self.assertEqual(len([sign for sign in device_capture.suite_failure_signs(result.stdout, 0)
                              if "SUITE_TEST_NAME_MAX" in sign]), 2)
        self.assertNotEqual(result.returncode, 0)

    def test_a_run_reporting_a_name_twice_fails(self):
        output = host_runner.run(FIXTURE, FIXTURE)
        self.assertEqual(device_capture.suite_failure_signs(output, 0),
                         [f"test name reported twice: {name}"
                          for name in ("test_fire_fits", "test_gas_fits", "test_water_fits")])


if __name__ == "__main__":
    unittest.main()
