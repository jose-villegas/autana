"""The Bakes workflow keeps one run per branch, its pushes' and its dispatches' in one group: a push cancels the run of
the head before it, and main's runs are never cancelled and all queue. Every job that makes a bake runs on the one
self-hosted runner that makes the fits, which no pull request starts, as a guarded stage in a venv of its own."""
import re
import sys
import unittest
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))
from scripts.lib import local_schedule  # noqa: E402
from scripts.gates import check_workflows  # noqa: E402

MAIN = "refs/heads/main"
BRANCH = "refs/heads/feature/x"


def evaluate(expression, ref):
    """A concurrency value, a literal or an expression that reads only github.ref, evaluated for `ref`."""
    match = re.fullmatch(r"\$\{\{(.*)\}\}", str(expression).strip())
    if match is None:
        return expression
    body = match.group(1)
    body = body.replace("github.ref", repr(ref)).replace("&&", " and ").replace("||", " or ")
    return eval(body, {"__builtins__": {}})


class OneRunPerBranch(unittest.TestCase):
    def setUp(self):
        self.concurrency = yaml.safe_load((ROOT / ".github/workflows/bakes.yml").read_text())["concurrency"]

    def test_the_group_is_the_branch(self):
        self.assertEqual(self.concurrency["group"], "bakes-${{ github.ref_name }}")

    def test_a_push_cancels_its_branchs_older_run_but_never_mains(self):
        self.assertTrue(evaluate(self.concurrency["cancel-in-progress"], BRANCH))
        self.assertFalse(evaluate(self.concurrency["cancel-in-progress"], MAIN))

    def test_mains_runs_all_queue_and_a_branch_keeps_one_pending(self):
        self.assertEqual(evaluate(self.concurrency["queue"], MAIN), "max")
        self.assertEqual(evaluate(self.concurrency["queue"], BRANCH), "single")

    def test_no_ref_asks_github_for_a_full_queue_with_cancelling(self):
        for ref in (MAIN, BRANCH):
            with self.subTest(ref=ref):
                self.assertFalse(evaluate(self.concurrency["queue"], ref) == "max"
                                 and evaluate(self.concurrency["cancel-in-progress"], ref))


RUNNER = ["self-hosted", "linux", "gpu-legion"]
VENV = '-m venv --system-site-packages "$RUNNER_TEMP/venv"'
# A bake.py command that makes bakes or plans them, and the join, which packs traced shards and traces nothing.
MAKES = re.compile(r"bake\.py (bake|shards)\b")
JOINS = re.compile(r"bake\.py bake --kind reference --shards ")


def runs(job):
    return "\n".join(step.get("run", "") for step in job.get("steps", []))


class OneRunnerMakesTheBakes(unittest.TestCase):
    def setUp(self):
        self.jobs = yaml.safe_load((ROOT / ".github/workflows/bakes.yml").read_text())["jobs"]
        self.making = {name: job for name, job in self.jobs.items()
                       if MAKES.search(runs(job)) and not JOINS.search(runs(job))}

    def test_every_job_that_makes_a_bake_runs_on_the_bakes_runner(self):
        self.assertEqual(set(self.making), {"cpu", "reference-plan", "reference-shard"})
        for name, job in self.making.items():
            with self.subTest(job=name):
                self.assertEqual(job["runs-on"], RUNNER)

    def test_no_pull_request_starts_the_workflow_and_the_gate_passes_it(self):
        workflow = yaml.load((ROOT / ".github/workflows/bakes.yml").read_text(), Loader=yaml.BaseLoader)
        self.assertEqual(set(workflow["on"]), {"push", "workflow_dispatch"})
        self.assertEqual([line for line in check_workflows.check(ROOT) if "bakes" in line], [])

    def test_a_runner_job_installs_into_its_own_venv_never_the_fit_environment(self):
        for name, job in self.jobs.items():
            if job["runs-on"] == RUNNER:
                with self.subTest(job=name):
                    self.assertIn(VENV, runs(job))
                    self.assertIn('echo "$RUNNER_TEMP/venv/bin" >> "$GITHUB_PATH"', runs(job))
                    self.assertNotIn('gpu/env/bin" >> "$GITHUB_PATH"', runs(job))
                    self.assertNotIn("gpu/env/bin/python\" -m pip", runs(job))

    def test_the_lock_check_runs_after_a_failed_bake(self):
        # A skipped required check passes, so the lock check must never skip for another job's failure.
        condition = self.jobs["lock"]["if"]
        for name in self.jobs["lock"]["needs"]:
            if name != "inputs":
                with self.subTest(job=name):
                    self.assertNotIn(f"needs.{name}.result", condition)

    def test_the_bakes_runner_installs_nothing_as_root(self):
        for name, job in self.jobs.items():
            if job["runs-on"] == RUNNER:
                with self.subTest(job=name):
                    self.assertNotIn("sudo", runs(job))

    def test_each_stage_names_a_guarded_workflow_the_job_outlives(self):
        for name, job in self.making.items():
            with self.subTest(job=name):
                self.assertIn("local_schedule.py start -- ", runs(job))
                self.assertIn(f"GPU_WORKFLOW={local_schedule.CPU_BAKES_WORKFLOW}", runs(job))
                self.assertLess(local_schedule.GPU_STAGE_HANG_SECONDS[local_schedule.CPU_BAKES_WORKFLOW],
                                job["timeout-minutes"] * 60)


if __name__ == "__main__":
    unittest.main()
