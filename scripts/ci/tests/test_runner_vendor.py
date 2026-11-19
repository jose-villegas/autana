"""runner_vendor fails a wrong-vendor runner first, and re-runs a run only for that failure."""
import contextlib
import io
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import runner_vendor  # noqa: E402

AMD, INTEL = "AuthenticAMD", "GenuineIntel"


def fake_api(attempt, jobs):
    """A request stand-in: `jobs` maps a name to (conclusion, annotation titles); POSTs are recorded."""
    posts = []
    table = {index: (name, *entry) for index, (name, entry) in enumerate(jobs.items())}

    def request(method, path, body=None):
        if method == "POST":
            posts.append(path)
            return {}
        if path.endswith("/annotations"):
            return [{"title": title} for title in table[int(path.split("/")[2])][2]]
        if "/jobs" in path:
            return {"jobs": [{"id": index, "name": name, "conclusion": conclusion}
                             for index, (name, conclusion, _) in table.items()]}
        return {"run_attempt": attempt}
    return request, posts


class RunnerVendorTests(unittest.TestCase):
    def check(self, lock, host):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = runner_vendor.check(lock, host)
        return code, out.getvalue()

    def test_a_runner_of_the_locks_vendor_traces(self):
        self.assertEqual(self.check(AMD, AMD)[0], 0)
        self.assertEqual(self.check(None, INTEL)[0], 0)

    def test_a_runner_of_another_vendor_fails_with_the_rerun_title(self):
        code, out = self.check(AMD, INTEL)
        self.assertEqual(code, 1)
        self.assertIn(f"::error title={runner_vendor.WRONG_VENDOR}::", out)

    def rerun(self, attempt, jobs):
        request, posts = fake_api(attempt, jobs)
        with contextlib.redirect_stdout(io.StringIO()):
            runner_vendor.rerun(7, request=request)
        return posts

    def test_a_run_failed_only_by_vendor_is_re_run(self):
        jobs = {"shard 2": ("failure", [runner_vendor.WRONG_VENDOR]), "shard 0": ("success", []),
                "join": ("skipped", [])}
        self.assertEqual(self.rerun(1, jobs), ["/actions/runs/7/rerun-failed-jobs"])

    def test_any_other_failure_is_left_for_a_person(self):
        jobs = {"shard 2": ("failure", [runner_vendor.WRONG_VENDOR]), "lock": ("failure", [None])}
        self.assertEqual(self.rerun(1, jobs), [])
        self.assertEqual(self.rerun(1, {"lock": ("success", [])}), [])

    def test_the_tries_are_capped(self):
        jobs = {"shard 2": ("failure", [runner_vendor.WRONG_VENDOR])}
        self.assertEqual(self.rerun(runner_vendor.VENDOR_TRIES - 1, jobs), ["/actions/runs/7/rerun-failed-jobs"])
        self.assertEqual(self.rerun(runner_vendor.VENDOR_TRIES, jobs), [])



def marker(shard, candidate, attempt, ok, created):
    return runner_vendor.marker_name(shard, candidate, attempt, ok), created


class ShardCandidateTests(unittest.TestCase):
    """Of a shard's candidates, one of the lock's vendor traces, and the shard fails only when none can."""

    def test_the_earliest_marked_on_the_locks_vendor_traces(self):
        artifacts = [marker(4, 2, 1, True, "10:00:01"), marker(4, 0, 1, True, "10:00:05")]
        self.assertEqual(runner_vendor.decide(artifacts, {0, 1, 2}, 4, 2, 1, True), runner_vendor.TRACE)
        self.assertEqual(runner_vendor.decide(artifacts, {0, 1, 2}, 4, 0, 1, True), runner_vendor.OTHER)

    def test_a_right_candidate_that_cannot_see_its_marker_yet_traces_alone(self):
        self.assertEqual(runner_vendor.decide([], {0, 1, 2}, 4, 1, 1, True), runner_vendor.TRACE)

    def test_a_wrong_candidate_finishes_green_while_a_sibling_can_still_trace(self):
        wrong = [marker(4, 0, 1, False, "10:00:00")]
        self.assertEqual(runner_vendor.decide(wrong + [marker(4, 1, 1, True, "10:00:02")], {0}, 4, 0, 1, False),
                         runner_vendor.OTHER)
        # Candidate 2's job has not marked yet: it may be the lock's vendor.
        self.assertEqual(runner_vendor.decide(wrong, {0, 2}, 4, 0, 1, False), runner_vendor.OTHER)

    def test_a_shard_is_lost_when_no_candidate_left_can_trace_it(self):
        wrong = [marker(4, 0, 1, False, "10:00:00"), marker(4, 1, 1, False, "10:00:03")]
        # Candidate 1 is still in its settle wait and candidate 2 finished green before candidate 1 marked.
        self.assertEqual(runner_vendor.decide(wrong, {0, 1}, 4, 0, 1, False), runner_vendor.LOST)
        # A re-run attempt runs only the failed candidate.
        self.assertEqual(runner_vendor.decide([marker(4, 1, 2, False, "11:00:00")], {1}, 4, 1, 2, False),
                         runner_vendor.LOST)

    def test_markers_of_another_shard_or_attempt_do_not_count(self):
        others = [marker(5, 1, 1, True, "10:00:00"), marker(4, 1, 1, True, "10:00:00")]
        self.assertEqual(runner_vendor.decide(others, {0}, 4, 0, 2, False), runner_vendor.LOST)
        self.assertEqual(runner_vendor.decide(others, {0}, 4, 0, 2, True), runner_vendor.TRACE)

    def test_a_shard_an_earlier_attempt_traced_is_done(self):
        artifacts = [("reference-shard-14-1", "10:00:00"), marker(4, 0, 1, True, "10:00:00")]
        self.assertFalse(runner_vendor.shard_done(artifacts, 4))
        self.assertTrue(runner_vendor.shard_done(artifacts + [("reference-shard-4-2", "11:00:00")], 4))

    def test_the_candidates_keep_the_matrix_under_its_cap(self):
        from bake import bake
        self.assertEqual(len(runner_vendor.candidates(1)), runner_vendor.SHARD_CANDIDATES)
        for count in (1, 18, bake.MATRIX_JOBS // 2, bake.MATRIX_JOBS):
            with self.subTest(count=count):
                self.assertLessEqual(count * len(runner_vendor.candidates(count)), bake.MATRIX_JOBS)
                self.assertGreaterEqual(len(runner_vendor.candidates(count)), 1)

    def claim(self, artifacts, jobs, marker_name, candidate=0):
        def request(method, path, body=None):
            if "/artifacts" in path:
                return {"artifacts": [{"name": name, "created_at": created} for name, created in artifacts]}
            return {"jobs": jobs}
        with tempfile.TemporaryDirectory() as folder:
            out_path = Path(folder) / "out"
            out_path.write_text("")
            env = {"GITHUB_RUN_ID": "7", "GITHUB_RUN_ATTEMPT": "1", "GITHUB_OUTPUT": str(out_path)}
            out = io.StringIO()
            with mock.patch.dict(os.environ, env), contextlib.redirect_stdout(out):
                code = runner_vendor.claim(4, candidate, marker_name, request=request, sleep=lambda _: None)
            return code, out.getvalue(), out_path.read_text()

    def test_a_lost_shard_fails_with_the_title_the_rerun_takes(self):
        name, created = marker(4, 0, 1, False, "10:00:00")
        jobs = [{"name": "Trace reference shard 4 (candidate 0)", "status": "in_progress"},
                {"name": "Trace reference shard 4 (candidate 1)", "status": "completed"},
                {"name": "Trace reference shard 3 (candidate 1)", "status": "queued"}]
        code, out, outputs = self.claim([(name, created)], jobs, name)
        self.assertEqual(code, 1)
        self.assertIn(f"::error title={runner_vendor.WRONG_VENDOR}::", out)
        self.assertEqual(outputs, "trace=false\n")
        self.assertTrue(runner_vendor.to_rerun(1, {"Trace reference shard 4 (candidate 0)": {runner_vendor.WRONG_VENDOR}}))

    def test_a_queued_sibling_keeps_a_wrong_candidate_green(self):
        name, created = marker(4, 0, 1, False, "10:00:00")
        jobs = [{"name": "Trace reference shard 4 (candidate 0)", "status": "in_progress"},
                {"name": "Trace reference shard 4 (candidate 2)", "status": "queued"}]
        code, _, outputs = self.claim([(name, created)], jobs, name)
        self.assertEqual((code, outputs), (0, "trace=false\n"))

    def test_the_tracing_candidate_says_so(self):
        name, created = marker(4, 1, 1, True, "10:00:00")
        code, _, outputs = self.claim([(name, created)], [], name, candidate=1)
        self.assertEqual((code, outputs), (0, "trace=true\n"))


class BakesWorkflowTests(unittest.TestCase):
    """A re-run happens only when the vendor check is a run's sole failure, so nothing after the lock may run,
    and fail, when a bake before it failed: a skipped lock is no failure."""

    def test_a_job_after_the_lock_waits_out_every_failure_the_lock_does(self):
        import yaml
        jobs = yaml.safe_load((runner_vendor.ROOT / ".github/workflows/bakes.yml").read_text())["jobs"]
        guarded = [name for name in jobs["lock"]["needs"] if f"needs.{name}.result != 'failure'" in jobs["lock"]["if"]]
        self.assertTrue(guarded)
        for name, job in jobs.items():
            needs = [job["needs"]] if isinstance(job.get("needs"), str) else job.get("needs", [])
            if "lock" not in needs:
                continue
            with self.subTest(job=name):
                for before in guarded:
                    self.assertIn(before, needs)
                    self.assertIn(f"needs.{before}.result != 'failure'", job["if"])

    def test_a_shard_candidate_job_is_named_as_claim_reads_it(self):
        import yaml
        job = yaml.safe_load((runner_vendor.ROOT / ".github/workflows/bakes.yml").read_text())["jobs"]["reference-shard"]
        name = job["name"].replace("${{ matrix.shard }}", "4").replace("${{ matrix.candidate }}", "2")
        match = runner_vendor.CANDIDATE_JOB.fullmatch(name)
        self.assertIsNotNone(match, job["name"])
        self.assertEqual((match[1], match[2]), ("4", "2"))

    def test_only_the_claiming_candidate_traces_and_uploads(self):
        import yaml
        steps = yaml.safe_load((runner_vendor.ROOT / ".github/workflows/bakes.yml").read_text())["jobs"]["reference-shard"]["steps"]
        names = [step.get("name") or step.get("uses") for step in steps]
        after = names.index("Whether this candidate traces the shard") + 1
        for step in steps[after:]:
            with self.subTest(step=step.get("name") or step.get("uses")):
                self.assertEqual(step.get("if"), "steps.claim.outputs.trace == 'true'")


if __name__ == "__main__":
    unittest.main()
