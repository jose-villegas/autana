"""The datasheet figures behind docs/power/Power-Budget.md: every row names its source and page, and the sums add up."""
import importlib.util
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]
SPEC = importlib.util.spec_from_file_location("power_budget_doc", ROOT / "launcher/tools/power/power_budget_doc.py")
doc = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(doc)


class PowerBudgetDocTest(unittest.TestCase):
    def setUp(self):
        self.data = doc.load()

    def test_every_figure_has_a_known_source_and_a_value(self):
        numbers = doc.citation_numbers()
        for name, figure in self.data["figures"].items():
            self.assertIn(figure["source"], self.data["sources"], name)
            self.assertIn(figure["source"], numbers, name)
            self.assertTrue(figure["page"] and figure["table"], name)
            self.assertIsNotNone(doc.basis(figure), name)
            self.assertIn(figure["block"], self.data["blocks"], name)

    def test_every_state_names_known_figures_and_one_amplifier_row(self):
        for state in self.data["states"]:
            blocks = [self.data["figures"][name]["block"] for name in state["figures"]]
            self.assertEqual(blocks.count(doc.AMP_BLOCK), 1, state["id"])

    def test_totals_follow_the_rows(self):
        state = next(s for s in self.data["states"] if s["id"] == "display-off")
        figures = [self.data["figures"][name] for name in state["figures"]]
        total = doc.totals(self.data, state)
        self.assertAlmostEqual(total["typ"], sum(f["typ"] for f in figures if f["typ"] is not None))
        self.assertEqual(total["no_typ"], 1)
        self.assertGreaterEqual(total["max"], total["typ"])

    def test_the_committed_document_is_current(self):
        self.assertFalse(doc.generate(check=True))


if __name__ == "__main__":
    unittest.main()
