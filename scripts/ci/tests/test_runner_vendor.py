"""runner_vendor fails a wrong-vendor runner first, and re-runs a run only for that failure."""
import contextlib
import io
import sys
import unittest
from pathlib import Path

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


if __name__ == "__main__":
    unittest.main()
