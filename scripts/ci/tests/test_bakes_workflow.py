"""The Bakes workflow keeps one run per branch, its pull request's and its dispatches' in one group: a push cancels
the run of the head before it, and main's runs are never cancelled and all queue."""
import re
import unittest
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parents[3]
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

    def test_the_group_is_the_heads_repository_and_branch(self):
        self.assertEqual(self.concurrency["group"], "bakes-${{ github.event.pull_request.head.repo.full_name || "
                                                    "github.repository }}-${{ github.head_ref || github.ref_name }}")

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


if __name__ == "__main__":
    unittest.main()
