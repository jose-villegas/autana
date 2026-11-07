"""batched_cost averages each scene and quarter over its runs and refuses a capture it cannot pair."""
import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
import batched_cost  # noqa: E402


def row(scene, mean, p95, quarter="portrait"):
    return (f"I (1) render_lab_profile: frame_profile: {scene} frames=700 mean {mean}us med {mean}us p95 {p95}us "
            f"worst {p95}us best {mean}us | fps mean 20.00 low 16.00 worst 15.00 quarter={quarter}")


def capture(*runs):
    return "\n".join(f"=== render.batched={batched} run {run} ===\n" + "\n".join(rows) for batched, run, rows in runs)


TWO_RUNS = capture(("1", 1, [row("sponza", 48000, 59000)]), ("0", 1, [row("sponza", 52000, 63000)]),
                   ("1", 2, [row("sponza", 50000, 61000)]), ("0", 2, [row("sponza", 54000, 65000)]))


class BatchedCostTests(unittest.TestCase):
    def test_a_scene_is_the_mean_of_its_runs(self):
        self.assertEqual(batched_cost.means(TWO_RUNS),
                         {("sponza", "portrait"): {"1": (49000, 60000), "0": (53000, 64000)}})

    def test_the_table_states_on_off_and_the_saving_in_ms(self):
        lines = batched_cost.render(TWO_RUNS).splitlines()
        self.assertEqual(lines[-1], "| sponza | portrait | 49.0 | 53.0 | 4.0 | 60.0 | 64.0 |")

    def test_unequal_runs_are_refused(self):
        with self.assertRaisesRegex(ValueError, "same number of runs"):
            batched_cost.means(TWO_RUNS + "\n" + capture(("1", 3, [row("sponza", 48000, 59000)])))

    def test_a_scene_under_one_setting_only_is_refused(self):
        extra = TWO_RUNS + "\n" + row("bistro", 36000, 50000)
        with self.assertRaisesRegex(ValueError, "bistro portrait lacks a setting"):
            batched_cost.means(extra)


if __name__ == "__main__":
    unittest.main()
