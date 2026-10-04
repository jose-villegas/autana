"""Self-hosted jobs must never accept untrusted pull request or comment events."""
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import check_self_hosted_workflows as gate


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


if __name__ == "__main__":
    unittest.main()
