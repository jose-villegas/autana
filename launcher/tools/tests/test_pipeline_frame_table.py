"""Frame tables follow capture order and require document owners."""
import pathlib
import sys
import tempfile
import unittest
from unittest.mock import patch
TOOLS = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
sys.path.insert(0, str(TOOLS / "render"))
from r3d import dynres_report
import frame_stages
from generated_blocks import apply_tables

class PipelineFrameTableTests(unittest.TestCase):
    def chart_axes(self, base, added):
        import matplotlib.pyplot as plt
        with tempfile.TemporaryDirectory() as directory, patch("matplotlib.pyplot.close", wraps=plt.close) as close:
            frame_stages.chart(list(base), base, added, pathlib.Path(directory) / "chart.png")
        return [call.args[0].axes[0] for call in close.call_args_list if hasattr(call.args[0], "axes")]

    def test_stage_headers_and_cells_share_resolve_column(self):
        base = {'mean': 1000, 'p50': 1000, 'max': 1000, 'r3d.draw': 2.0, 'r3d.upscale': 3.0}
        spans = dict(setup=100, rows=100, span_setup=100, fill=100)
        table = dynres_report.stages_table({(184, 224): dict(base, **{'r3d.resolve': 4.0}),
                                          (92, 112): base}, {(184, 224): spans, (92, 112): spans})
        lines = table.splitlines()
        index = lines[0].split('|').index(' resolve ')
        self.assertEqual(lines[2].split('|')[index].strip(), '4.0')
        self.assertEqual(lines[3].split('|')[index].strip(), 'not in capture')
        self.assertTrue(all(len(line.split('|')) == len(lines[0].split('|')) for line in lines[:4]))

    def test_views_add_their_difference_in_report_order(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            (root / "shipped.log").write_text("ms/frame avg/worst: r3d.draw 40/41 present.wait 0/0 frame.rest 1/1 | total 41\n")
            (root / "view-motion.log").write_text(
                "ms/frame avg/worst: r3d.draw 42/43 r3d.resolve 15/15 present.wait 0/0 frame.rest 7/7 | total 64\n")
            order, base, added = frame_stages.stage_costs(root / "shipped.log", str(root / "view-{view}.log"),
                                                    ["depth", "motion"])
            self.assertEqual(order, ["r3d.draw", "r3d.resolve", "present.wait", "frame.rest"])
            self.assertNotIn("r3d.resolve", base)
            self.assertEqual(list(added), ["motion"])
            self.assertEqual(added["motion"], {"r3d.draw": 2, "r3d.resolve": 15, "present.wait": 0, "frame.rest": 6})
            table = frame_stages.table(order, base, added, ["depth", "motion"], [1, 1], "chart.png")
            self.assertIn("| r3d.resolve | not run | not run | +15.00 |", table)
            self.assertIn("| total | 41.00 | 100% | +23.00 |", table)
            self.assertIn("Not captured: depth.", table)
            self.assertIn("mean of 1 report windows", table)
            self.assertIn("mean of 1 to 3 report windows",
                          frame_stages.table(order, base, added, ["motion"], [3, 1], "chart.png"))
            (root / "view-motion.log").write_text(
                "ms/frame avg/worst: r3d.draw 30/31 r3d.show 2/2 tail.shared 3/3 motion.tail 1/1 | total 36\n")
            (root / "view-tiles.log").write_text(
                "ms/frame avg/worst: r3d.draw 40/41 r3d.show 1/1 tail.shared 2/2 tiles.tail 1/1 | total 44\n")
            order, base, added = frame_stages.stage_costs(root / "shipped.log", str(root / "view-{view}.log"),
                                                        ["depth", "motion", "tiles"])
            self.assertEqual(order, ["r3d.draw", "present.wait", "frame.rest", "r3d.show",
                                     "tail.shared", "motion.tail", "tiles.tail"])
            self.assertEqual(list(added), ["motion", "tiles"])
            self.assertEqual(added["motion"]["r3d.draw"], -10)
            self.assertEqual(added["motion"]["frame.rest"], -1)
            table = frame_stages.table(order, base, added, ["depth", "motion", "tiles"], [1, 1, 1], "chart.png")
            self.assertIn("| r3d.draw | 40.00 | 98% | -10.00 | +0.00 |", table)
            self.assertIn("| frame.rest | 1.00 | 2% | -1.00 | -1.00 |", table)
            self.assertIn("| r3d.show | not run | not run | +2.00 | +1.00 |", table)
            self.assertIn("| total | 41.00 | 100% | -5.00 | +3.00 |", table)
            with self.assertRaises(ValueError):
                frame_stages.stage_costs(root / "missing.log", str(root / "view-{view}.log"), ["motion"])

    def test_each_captured_view_has_its_own_chart(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            shipped = root / "frame.png"
            order = ["r3d.draw", "r3d.show", "present.wait", "frame.rest"]
            base = {"r3d.draw": 40, "present.wait": 0, "frame.rest": 1}
            added = {"motion": {"r3d.draw": -10, "r3d.show": 2, "present.wait": 0, "frame.rest": -1},
                     "tiles": {"r3d.draw": 1, "r3d.show": 3, "present.wait": 0, "frame.rest": 0}}
            names = ["depth", "motion", "tiles"]
            import matplotlib.pyplot as plt
            with patch("matplotlib.pyplot.close", wraps=plt.close) as close:
                frame_stages.chart(order, base, added, shipped)
            axes = [call.args[0].axes[0] for call in close.call_args_list
                    if hasattr(call.args[0], "axes")]
            self.assertEqual([axis.get_title() for axis in axes],
                             ["As shipped", "With the motion view", "With the tiles view"])
            self.assertEqual(axes[0].get_xlim(), (0, max(sum(base.values()),
                             max(frame_stages.MS_PER_SECOND / rate for rate in frame_stages.FRAME_RATES))
                             * (1 + frame_stages.AXIS_HEADROOM)))
            self.assertNotEqual(axes[1].get_xlim(), axes[2].get_xlim())
            for axis in axes[1:]:
                self.assertTrue(all(text.get_text().startswith("+") and text.get_text() != "+0"
                                    for text in axis.get_xticklabels()))
                arrows = [text for text in axis.texts if getattr(text, "arrow_patch", None) is not None]
                self.assertEqual(len(arrows), 1)
                start = sum(base.values())
                self.assertEqual(axis.get_xlim()[0], start)
                self.assertEqual(arrows[0].xy, (start, -frame_stages.BAR_HEIGHT / 2))
                self.assertEqual(arrows[0].get_text(), frame_stages.ADDED_BASE_LABEL)
                self.assertGreater(arrows[0].get_position()[0], start)
                self.assertEqual(arrows[0].get_position()[1], -frame_stages.BAR_HEIGHT)
                self.assertEqual(arrows[0].get_fontsize(), frame_stages.BUDGET_LABEL_SIZE)
                self.assertFalse(any("41.0" in text.get_text() for text in axis.texts))
            self.assertEqual([text.get_text() for text in axes[1].texts if " ms" in text.get_text()][:1], ["-9.0 ms (-22%)"])
            legends = [axis.figure.legends[0] for axis in axes]
            self.assertEqual([[text.get_text() for text in legend.texts] for legend in legends],
                             [["r3d.draw 98%", "frame.rest 2%"], ["r3d.show +5%"], ["r3d.draw +2%", "r3d.show +7%"]])
            from matplotlib.colors import to_rgba
            self.assertEqual(axes[0].patches[1].get_facecolor(), to_rgba(frame_stages.SLOTS[2]))
            self.assertEqual(axes[1].patches[0].get_facecolor()[:3], to_rgba(frame_stages.SLOTS[3])[:3])
            self.assertEqual(axes[0].patches[0].get_x(), 0)
            for axis in axes[1:]:
                self.assertEqual(axis.patches[0].get_x(), sum(base.values()))
                self.assertTrue(all(bar.get_x() >= sum(base.values()) and bar.get_hatch()
                                    for bar in axis.patches))
            for axis in axes:
                self.assertEqual([line.get_xdata()[0] for line in axis.lines if line.get_transform() != axis.transAxes],
                                 [frame_stages.MS_PER_SECOND / rate for rate in frame_stages.FRAME_RATES
                                  if axis.get_xlim()[0] <= frame_stages.MS_PER_SECOND / rate <= axis.get_xlim()[1]])
            self.assertEqual({path.name for path in root.glob("*.png")},
                             {"frame.png", "frame.motion.png", "frame.tiles.png"})
            table = frame_stages.table(order, base, added, names, [1, 1, 1], shipped.name)
            images = [line for line in table.splitlines() if line.startswith("![")]
            self.assertEqual(images, [
                "![Frame stages as shipped](../images/render/frame.png)",
                "![Frame stages with the motion view](../images/render/frame.motion.png)",
                "![Frame stages with the tiles view](../images/render/frame.tiles.png)"])
            self.assertIn("Not captured: depth.", table)

    def test_stage_shares_use_shipped_total_before_rounding(self):
        base = {"r3d.draw": 7, "r3d.cull": 2, "frame.rest": 3}
        shipped = sum(base.values())
        shares = frame_stages.stage_shares(base, shipped)
        self.assertAlmostEqual(sum(shares.values()), frame_stages.PERCENT_SCALE)
        self.assertAlmostEqual(shares["r3d.draw"], 7 * frame_stages.PERCENT_SCALE / shipped)
        added = frame_stages.stage_shares({"r3d.draw": 3, "frame.rest": -1}, shipped)
        self.assertEqual(added, {"r3d.draw": 25, "frame.rest": -frame_stages.PERCENT_SCALE / shipped})
        self.assertEqual(frame_stages.share_text(added["r3d.draw"], signed=True), "+25%")
        self.assertEqual(frame_stages.share_text(added["frame.rest"], signed=True), "-8%")

    def test_segment_labels_and_thin_segment_legends_carry_shares(self):
        base = {"r3d.draw": 49.5, "r3d.cull": 0.5}
        added = {"depth": {"r3d.draw": 20, "r3d.cull": 0.5}}
        axes = self.chart_axes(base, added)
        self.assertIn("49.5 · 99%", [text.get_text() for text in axes[0].texts])
        self.assertIn("20.0 · +40%", [text.get_text() for text in axes[1].texts])
        self.assertIn("+20.5 ms (+41%)", [text.get_text() for text in axes[1].texts])
        self.assertEqual([[text.get_text() for text in axis.figure.legends[0].texts] for axis in axes],
                         [["r3d.draw 99%", "r3d.cull 1%"], ["r3d.draw +40%", "r3d.cull +1%"]])
        table = frame_stages.table(list(base), base, added, ["depth"], [1, 1], "chart.png")
        self.assertIn("| Stage | As shipped | Share | depth view adds |", table)
        self.assertIn("| r3d.cull | 0.50 | 1% | +0.50 |", table)

    def test_zero_rounded_shares_are_absent_from_bars_and_legends(self):
        base = {"r3d.draw": 99.6, "r3d.cull": 0.4}
        added = {"motion": {"r3d.draw": 20, "r3d.cull": 0.4}}
        axes = self.chart_axes(base, added)
        for axis, value, label in zip(axes, [99.6, 20], ["r3d.draw 100%", "r3d.draw +20%"]):
            with self.subTest(chart=axis.get_title()):
                self.assertEqual([bar.get_width() for bar in axis.patches], [value])
                self.assertEqual([text.get_text() for text in axis.figure.legends[0].texts], [label])
        table = frame_stages.table(list(base), base, added, ["motion"], [1, 1], "chart.png")
        self.assertIn("| r3d.cull | 0.40 | 0% | +0.40 |", table)

    def test_negative_delta_is_absent_from_bars_and_legend(self):
        base = {"r3d.draw": 90, "frame.rest": 10}
        added = {"motion": {"r3d.draw": 20, "frame.rest": -5}}
        axis = self.chart_axes(base, added)[-1]
        self.assertEqual([bar.get_width() for bar in axis.patches], [20])
        self.assertEqual([text.get_text() for text in axis.figure.legends[0].texts], ["r3d.draw +20%"])

    def test_view_legend_handles_match_drawn_bar_styles(self):
        base = {"r3d.draw": 90, "frame.rest": 10}
        added = {"motion": {"r3d.draw": 20, "frame.rest": 5}}
        axes = self.chart_axes(base, added)
        for axis in axes:
            handles = axis.figure.legends[0].legend_handles
            self.assertEqual(len(handles), len(axis.patches))
            for handle, bar in zip(handles, axis.patches):
                with self.subTest(chart=axis.get_title(), colour=bar.get_facecolor()):
                    self.assertEqual(handle.get_hatch(), bar.get_hatch())
                    self.assertEqual(handle.get_facecolor(), bar.get_facecolor())
                    self.assertEqual(handle.get_alpha(), bar.get_alpha())

    def test_view_names_come_from_the_view_table(self):
        names = frame_stages.view_names()
        self.assertEqual(names, ["depth", "tiles", "motion", "meshlets"])
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "context.c"
            source.write_text('static const render_debug_view_t views[] = {\n'
                              '    {"tiles", 0, tiles},\n    {"depth", 0, depth},\n};\n'
                              'static const other_t later[] = {{"x", 0, x}};\n')
            self.assertEqual(frame_stages.view_names(source), ["tiles", "depth"])
            source.write_text("static int x;\n")
            with self.assertRaises(ValueError):
                frame_stages.view_names(source)

    def test_every_table_needs_a_document_owner(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            (root / "bake-steps.md").write_text("table\n")
            with patch("generated_blocks.tracked_files", return_value=[]), self.assertRaises(ValueError):
                apply_tables(root, root)
