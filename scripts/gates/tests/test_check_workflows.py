"""Self-hosted jobs never take untrusted events; a group shared across branches queues every pending run."""
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import check_workflows as gate


class SelfHostedWorkflowTests(unittest.TestCase):
    def check(self, text):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            workflows = root / ".github/workflows"
            workflows.mkdir(parents=True)
            (workflows / "any-name.yml").write_text(text)
            return gate.check(root)

    def workflow(self, trigger, runner="[self-hosted, linux, gpu]"):
        return f"on: {trigger}\njobs:\n  run:\n    runs-on: {runner}\n"

    def test_rejects_every_untrusted_trigger_and_runner_form(self):
        for event in ("pull_request", "pull_request_target", "issue_comment",
                      "pull_request_review", "pull_request_review_comment", "pull_request_review_thread"):
            for trigger in (event, f"[push, {event}]", f"\n  {event}:\n    types: [opened]"):
                for runner in ("self-hosted", "'self-hosted'", "[linux, self-hosted, gpu]"):
                    with self.subTest(event=event, trigger=trigger, runner=runner):
                        self.assertTrue(self.check(self.workflow(trigger, runner)))

    def test_allows_trusted_events(self):
        self.assertEqual(self.check(self.workflow("\n  workflow_dispatch:\n  schedule:\n    - cron: '0 3 * * 0'\n  push:\n    branches: [main]")), [])

    def test_hosted_jobs_can_run_on_pull_requests(self):
        self.assertEqual(self.check(self.workflow("pull_request", "ubuntu-latest")), [])

    def test_checks_every_job_and_yaml_aliases(self):
        text = "on: [push, issue_comment]\nx-runner: &runner [linux, self-hosted]\njobs:\n  hosted:\n    runs-on: ubuntu-latest\n  local:\n    runs-on: *runner\n"
        self.assertTrue(self.check(text))

    def test_invalid_yaml_fails_closed(self):
        with self.assertRaises(ValueError):
            self.check("on: [pull_request\n")

    def test_checks_every_workflow_file(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            workflows = root / ".github/workflows"
            workflows.mkdir(parents=True)
            for name in ("first.yml", "second.yml"):
                (workflows / name).write_text(self.workflow("pull_request"))
            self.assertEqual(len(gate.check(root)), 2)

    def concurrency(self, block, job_level):
        if job_level:
            lines = "".join(f"\n      {line}" for line in block)
            return f"on: workflow_dispatch\njobs:\n  run:\n    runs-on: [self-hosted, linux, gpu]\n    concurrency:{lines}\n"
        lines = "".join(f"\n  {line}" for line in block)
        return f"on: workflow_dispatch\nconcurrency:{lines}\njobs:\n  run:\n    runs-on: ubuntu-latest\n"

    def test_a_shared_group_without_a_queue_is_rejected_in_every_form(self):
        for block in (["group: gpu"], ["group: gpu", "cancel-in-progress: false"],
                      ["group: gpu-${{ inputs.runner }}", "cancel-in-progress: false"]):
            for job_level in (False, True):
                with self.subTest(block=block, job_level=job_level):
                    self.assertTrue(self.check(self.concurrency(block, job_level)))
        self.assertTrue(self.check("on: workflow_dispatch\nconcurrency: gpu\njobs:\n  run:\n    runs-on: ubuntu-latest\n"))

    def test_queued_per_branch_or_cancelling_groups_pass(self):
        for block in (["group: gpu", "queue: max"], ["group: gpu", "cancel-in-progress: false", "queue: max"],
                      ["group: x-${{ github.ref }}"], ["group: x-${{ github.head_ref || github.run_id }}"], ["group: x-${{ github.sha }}"],
                      ["group: x-${{ github.event.pull_request.number }}"],
                      ["group: gpu", "cancel-in-progress: true"],
                      ["group: gpu", "cancel-in-progress: ${{ github.event_name == 'push' }}"]):
            for job_level in (False, True):
                with self.subTest(block=block, job_level=job_level):
                    self.assertEqual(self.check(self.concurrency(block, job_level)), [])


    def test_a_step_installs_packages_only_through_the_apt_script(self):
        steps = ("on: push\njobs:\n  run:\n    runs-on: ubuntu-latest\n    steps:\n"
                 "      - name: deps\n        run: |\n")
        for raw in ("sudo apt-get update", "sudo apt-get install -y llvm", "apt-get -qq update",
                    "sudo apt install -y ffmpeg"):
            with self.subTest(raw=raw):
                self.assertTrue(self.check(steps + f"          {raw}\n"))
        self.assertEqual(self.check(steps + f"          sudo sh {gate.APT_INSTALL} -y llvm\n"), [])


if __name__ == "__main__":
    unittest.main()
