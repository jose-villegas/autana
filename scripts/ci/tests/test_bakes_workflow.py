"""The Bakes workflow keeps one run per branch, its pull request's and its dispatches' in one group: a push cancels
the run of the head before it, and main's runs are never cancelled."""
import unittest
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parents[3]


class OneRunPerBranch(unittest.TestCase):
    def test_a_push_cancels_its_branchs_older_run_but_never_mains(self):
        concurrency = yaml.safe_load((ROOT / ".github/workflows/bakes.yml").read_text())["concurrency"]
        self.assertEqual(concurrency["group"], "bakes-${{ github.head_ref || github.ref_name }}")
        self.assertEqual(concurrency["cancel-in-progress"], "${{ github.ref != 'refs/heads/main' }}")


if __name__ == "__main__":
    unittest.main()
