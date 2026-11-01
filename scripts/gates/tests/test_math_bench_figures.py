"""Benchmark panels pair annotation rows and preserve the measurement scale."""
from pathlib import Path
import sys
import unittest
from unittest.mock import patch
from matplotlib.colors import to_rgba

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "launcher/tools/math"))


class MathBenchFigureTests(unittest.TestCase):
    def paired_figure(self, rows, measured):
        import math_bench_figures as figures
        from types import SimpleNamespace
        import matplotlib.pyplot as plt
        fig = figures.render(rows, measured, [SimpleNamespace(fast=rows[0][0], plain=rows[1][0])])
        self.addCleanup(plt.close, fig)
        return fig

    def test_committed_pairs_families_and_zero_axes(self):
        import math_bench_figures as figures
        rows, _ = figures.bench.read_fpu(figures.bench.DATA / "math_bench_fpu_ops.txt")
        items = [item for item in figures.fast_paths.annotations() if item.unit != "bit"]
        _, measured = figures.bench.parse_capture(
            (figures.bench.DATA / "math_bench_capture.txt").read_text(), rows)
        references = {item.plain for item in items}
        ordinary = [row for row in rows if row[0] not in references and row[4] != "calibration"]

        def check_plot(fig, path):
            self.assertEqual(path.name, "benchmark-families.png")
            self.assertEqual([axis.get_title(loc="left") for axis in fig.axes],
                             ["Vector", "Scalar", "Quaternion", "Matrix", "libm (no fast path)"])
            bars = [bar for axis in fig.axes for bar in axis.patches]
            self.assertEqual(len(bars), len(ordinary) + len(items))
            hatched = [bar for bar in bars if bar.get_hatch() == figures.REFERENCE_HATCH]
            self.assertEqual(len(hatched), len(items))
            self.assertCountEqual([text.get_text() for axis in fig.axes for text in axis.texts
                                   if text.get_text().isdigit()],
                                  [f"{measured[row[0]][0]:.0f}" for row in ordinary] +
                                  [f"{measured[item.plain][0]:.0f}" for item in items])
            self.assertEqual([[tick.get_text() for tick in axis.get_xticklabels()] for axis in fig.axes],
                             [list(groups) for _, groups in figures.panel_groups(rows, references)])
            for axis in fig.axes:
                self.assertEqual(axis.get_ylim()[0], 0)
                self.assertEqual(axis.get_yscale(), "linear")
            import matplotlib.pyplot as plt
            plt.close(fig)

        with patch.object(figures, "save", side_effect=check_plot):
            self.assertEqual(figures.main([]), 0)

    def test_pairing_uses_annotations_and_skips_empty_panels(self):
        import math_bench_figures as figures
        rows = [("a_f", "chosen", "f", 1, "scalar"),
                ("b_i", "reference", "i", 1, "libm")]
        measured = {"a_f": (5, 6, 1), "b_i": (2, 3, 1)}
        fig = self.paired_figure(rows, measured)
        self.assertEqual(len(fig.axes), 1)
        axis = fig.axes[0]
        self.assertEqual([tick.get_text() for tick in axis.get_xticklabels()], ["chosen"])
        fast, plain = axis.patches
        self.assertLess(fast.get_x(), plain.get_x())
        self.assertEqual(plain.get_height(), measured["b_i"][0])
        self.assertEqual(plain.get_facecolor(), to_rgba(figures.REFERENCE_FILL))
        self.assertEqual(len(axis.lines), 0)
        self.assertEqual(plain.get_edgecolor(), to_rgba(figures.SLOTS[1]))
        self.assertIn("reference (i)", [text.get_text() for text in axis.texts])

    def test_tall_reference_preserves_panel_scale_and_true_value(self):
        import math_bench_figures as figures
        rows = [("fast_x", "normalize", "x", 1, "vector"),
                ("plain_x", "normalize_div", "x", 1, "vector")]
        measured = {"fast_x": (100, 100, 1), "plain_x": (1115, 1115, 1)}
        fig = self.paired_figure(rows, measured)
        axis = fig.axes[0]
        limit = measured["fast_x"][0] * figures.VALUE_HEADROOM
        self.assertEqual(axis.get_ylim(), (0, limit))
        self.assertEqual(axis.get_yscale(), "linear")
        self.assertEqual(axis.patches[1].get_height(), limit)
        value = next(text for text in axis.texts if text.get_text() == "1115")
        name = next(text for text in axis.texts if text.get_text() == "normalize_div (x)")
        self.assertEqual(value.xy[1], limit)
        self.assertEqual(name.xy[1], limit)
        self.assertGreater(name.get_position()[1], value.get_position()[1])
        self.assertEqual(len(axis.lines), 2)
        for line in axis.lines:
            self.assertEqual(line.get_color(), figures.REFERENCE_FILL)
            self.assertTrue(all(0 < y < limit for y in line.get_ydata()))

    def test_near_limit_reference_grows_panel_without_break(self):
        import math_bench_figures as figures
        rows = [("fast_i", "sine (angle)", "i", 1, "scalar"),
                ("plain_f", "sinf", "f", 1, "libm")]
        measured = {"fast_i": (157, 157, 1), "plain_f": (224, 224, 1)}
        fig = self.paired_figure(rows, measured)
        axis = fig.axes[0]
        self.assertGreater(axis.get_ylim()[1], 224)
        self.assertEqual(axis.patches[1].get_height(), 224)
        self.assertEqual(len(axis.lines), 0)
        self.assertEqual([tick.get_text() for tick in axis.get_xticklabels()], ["sine"])
        fig.canvas.draw()
        renderer = fig.canvas.get_renderer()
        for text in axis.texts:
            self.assertLess(text.get_window_extent(renderer).y1, axis.get_window_extent(renderer).y1)

    def test_row_types_define_palette_order_including_unknown_type(self):
        import math_bench_figures as figures
        rows = [("z", "sample", "z", 1, "scalar"),
                ("i", "sample", "i", 1, "scalar"),
                ("convert", "convert", "f_to_z", 1, "scalar"),
                ("f", "sample", "f", 1, "scalar")]
        fig = figures.render(rows, {row[0]: (1, 1, 1) for row in rows}, [])
        import matplotlib.pyplot as plt
        self.addCleanup(plt.close, fig)
        self.assertEqual([bar.get_facecolor() for bar in fig.axes[0].patches],
                         [to_rgba(figures.SLOTS[index]) for index in (0, 1, 2, 2)])
        self.assertEqual([text.get_text() for text in fig.legends[0].get_texts()],
                         ["f", "", "i", "", "z", ""])

    def test_reference_cut_threshold_boundaries(self):
        import math
        import math_bench_figures as figures
        rows = [("fast", "chosen", "f", 1, "scalar"),
                ("plain", "reference", "f", 1, "scalar")]
        warm = 100
        limit = warm * figures.VALUE_HEADROOM
        threshold = limit * figures.REFERENCE_CUT_FACTOR
        for reference, expected_limit, breaks in (
                (limit, limit, 0),
                (threshold, threshold * figures.REFERENCE_HEADROOM, 0),
                (math.nextafter(threshold, math.inf), limit, 2)):
            with self.subTest(reference=reference):
                fig = self.paired_figure(rows, {"fast": (warm, warm, 1), "plain": (reference, reference, 1)})
                axis = fig.axes[0]
                self.assertEqual(axis.get_ylim()[1], expected_limit)
                self.assertEqual(axis.patches[1].get_height(), min(reference, expected_limit))
                self.assertEqual(len(axis.lines), breaks)
                value = next(text for text in axis.texts if text.get_text() == f"{reference:.0f}")
                self.assertEqual(value.xy[1], min(reference, expected_limit))

    def test_group_width_bounds_and_small_group_bar_width(self):
        import math_bench_figures as figures
        import matplotlib.pyplot as plt
        for count in (1, 2, 8):
            with self.subTest(count=count):
                rows = [(f"sample_{index}", "sample", "f", 1, "scalar") for index in range(count)]
                rows.append(("next", "next", "f", 1, "scalar"))
                fig = figures.render(rows, {row[0]: (1, 1, 1) for row in rows}, [])
                self.addCleanup(plt.close, fig)
                bars = fig.axes[0].patches
                left = min(bar.get_x() for bar in bars[:count])
                right = max(bar.get_x() + bar.get_width() for bar in bars[:count])
                self.assertAlmostEqual(max(0, right - left - figures.GROUP_WIDTH), 0)
                self.assertAlmostEqual(max(0, -figures.GROUP_WIDTH / 2 - left), 0)
                self.assertAlmostEqual(max(0, right - figures.GROUP_WIDTH / 2), 0)
                self.assertLess(right, bars[count].get_x())
                if count <= 2:
                    for bar in bars[:count]:
                        self.assertAlmostEqual(bar.get_width(), figures.BAR_WIDTH)

    def test_synthetic_calibration_is_excluded_and_families_remain(self):
        import math_bench_figures as figures
        import matplotlib.pyplot as plt
        families = [family for family in figures.bench.ROW_FAMILIES if family != "calibration"]
        rows = [(family, family, "f", 1, family) for family in families]
        rows.append(("calibration", "calibration", "f", 1, "calibration"))
        fig = figures.render(rows, {row[0]: (1, 1, 1) for row in rows}, [])
        self.addCleanup(plt.close, fig)
        self.assertEqual([axis.get_title(loc="left") for axis in fig.axes],
                         ["libm (no fast path)" if family == "libm" else family.capitalize() for family in families])
        self.assertEqual([tick.get_text() for axis in fig.axes for tick in axis.get_xticklabels()], families)
        self.assertEqual(sum(len(axis.patches) for axis in fig.axes), len(families))
