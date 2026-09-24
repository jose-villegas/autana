import sys
import unittest
from pathlib import Path

AUTANA = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(AUTANA))
import autana  # noqa: E402


class CompletionTests(unittest.TestCase):
    def test_first_word_completes_command_names(self):
        self.assertEqual(autana.completion_candidates("fl", "fl"), ["flash"])

    def test_flash_second_word_completes_variants(self):
        self.assertEqual(autana.completion_candidates("flash d", "d"), ["dev", "diag"])

    def test_hyphenated_command_completes_whole(self):
        self.assertEqual(autana.completion_candidates("take", "take"), ["take-back"])

    def test_console_does_not_offer_itself(self):
        self.assertNotIn("console", autana.completion_candidates("", ""))

    def test_unknown_prefix_has_no_candidates(self):
        self.assertEqual(autana.completion_candidates("unknown x", "x"), [])


if __name__ == "__main__":
    unittest.main()
