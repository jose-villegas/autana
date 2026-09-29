import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "device" / "tests"))
import isolation  # noqa: E402,F401  (first: keeps the suite out of real records)
import unittest
import os


AUTANA = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(AUTANA))
import autana  # noqa: E402


class CompletionTests(unittest.TestCase):
    def test_first_word_completes_command_names(self):
        self.assertEqual(autana.completion_candidates("fl", "fl"), ["flash"])

    def test_build_second_word_completes_variants(self):
        self.assertEqual(autana.completion_candidates("build d", "d"), ["dev", "diag"])
        self.assertEqual(autana.completion_candidates("build r", "r"), ["rel", "release"])

    def test_flash_second_word_completes_variants(self):
        self.assertEqual(autana.completion_candidates("flash d", "d"), ["dev", "diag"])

    def test_the_old_lock_spellings_are_not_offered(self):
        offered = autana.completion_candidates("", "")
        for word in ("id", "release", "hand", "take-back"):
            self.assertNotIn(word, offered)

    def test_console_does_not_offer_itself(self):
        self.assertNotIn("console", autana.completion_candidates("", ""))

    def test_unknown_prefix_has_no_candidates(self):
        self.assertEqual(autana.completion_candidates("unknown x", "x"), [])


if __name__ == "__main__":
    unittest.main()
