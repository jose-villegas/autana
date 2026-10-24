"""Benchmark panels pair annotation rows and preserve the measurement scale."""
from pathlib import Path
import sys
import unittest
from unittest.mock import patch
from matplotlib.colors import to_rgba

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "launcher/tools/math"))


class MathBenchFigureTests(unittest.TestCase):
    def test_committed_pairs_families_and_zero_axes(self):
        import math_bench_figures as figures
        rows, _ = figures.bench.read_fpu(figures.bench.DATA / "math_bench_fpu_ops.txt")
        items = figures.fast_paths.annotations()
        _, measured = figures.bench.parse_capture(
            (figures.bench.DATA / "math_bench_capture.txt").read_text(), rows)
        references = {item.plain for item in items}
        ordinary = [row for row in rows if row[0] not in references and row[4] != "calibration"]

        def check_plot(fig, path):
            self.assertEqual(path.name, "benchmark-families.png")
            self.assertEqual([axis.get_title(loc="left") for axis in fig.axes],
                             [family.capitalize() if family != "libm" else "libm (no fast path)"
                              for family in figures.bench.ROW_FAMILIES if family != "calibration"])
            bars = [bar for axis in fig.axes for bar in axis.patches]
            self.assertEqual(len(bars), len(ordinary) + len(items))
            hatched = [bar for bar in bars if bar.get_hatch() == "///"]
            self.assertCountEqual([bar.get_height() for bar in hatched],
                                  [measured[item.plain][0] for item in items])
            labels = [tick.get_text() for axis in fig.axes for tick in axis.get_xticklabels()]
            self.assertNotIn("dot_f32_n256", labels)
            for key, label, kind, _, _ in rows:
                if key in references:
                    self.assertNotIn(label, labels)
                if "_to_" in kind:
                    self.assertIn("convert " + kind.replace("_to_", "\u2192"), labels)
            for axis in fig.axes:
                self.assertEqual(axis.get_ylim()[0], 0)
                self.assertEqual(axis.get_yscale(), "linear")
            import matplotlib.pyplot as plt
            plt.close(fig)

        with patch.object(figures, "save", side_effect=check_plot):
            self.assertEqual(figures.main([]), 0)

    def test_pairing_uses_annotations_and_skips_empty_panels(self):
        import math_bench_figures as figures
        from types import SimpleNamespace
        rows = [("a_f", "chosen", "f", 1, "scalar"),
                ("b_i", "reference", "i", 1, "libm")]
        measured = {"a_f": (2, 3, 1), "b_i": (5, 6, 1)}
        items = [SimpleNamespace(fast="a_f", plain="b_i")]
        fig = figures.render(rows, measured, items)
        self.assertEqual(len(fig.axes), 1)
        axis = fig.axes[0]
        self.assertEqual([tick.get_text() for tick in axis.get_xticklabels()], ["chosen"])
        fast, plain = axis.patches
        self.assertLess(fast.get_x(), plain.get_x())
        self.assertEqual(plain.get_edgecolor(), to_rgba(figures.SLOTS[1]))
        self.assertIn("reference (i)", [text.get_text() for text in axis.texts])
        import matplotlib.pyplot as plt
        plt.close(fig)
