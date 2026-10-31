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

    def test_stage_figure_keeps_resolve_only_when_captured(self):
        base = {'mean': 1000, 'p50': 1000, 'max': 1000, 'r3d.draw': 2.0, 'r3d.upscale': 3.0}
        spans = dict(setup=100, edges=100, carry=100, fill=100)
        splits = {(184, 224): dict(base, **{'r3d.resolve': 4.0}), (92, 112): base}
        with tempfile.TemporaryDirectory() as directory, patch("docs.plot_style.save_themes") as save:
            dynres_report.stage_figures(splits, spans, directory)
        stages = [text.get_text() for text in save.call_args_list[0].args[0].axes[0].get_legend().get_texts()]
        self.assertIn("resolve", stages)
        self.assertEqual(len(dynres_report.stages_table(splits).splitlines()), 6)

    def test_views_add_their_difference_in_report_order(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            (root / "shipped.log").write_text("ms/frame avg/worst: stage.a 40/41 stage.idle 0/0 stage.rest 1/1 | total 41\n")
            (root / "view-motion.log").write_text(
                "ms/frame avg/worst: stage.a 42/43 view.only 15/15 stage.idle 0/0 stage.rest 7/7 | total 64\n")
            order, base, added = frame_stages.stage_costs(root / "shipped.log", str(root / "view-{view}.log"),
                                                    ["depth", "motion"])
            self.assertEqual(order, ["stage.a", "view.only", "stage.idle", "stage.rest"])
            self.assertNotIn("view.only", base)
            self.assertEqual(list(added), ["motion"])
            self.assertEqual(added["motion"], {"stage.a": 2, "view.only": 15, "stage.idle": 0, "stage.rest": 6})
            table = frame_stages.table(order, base, added, ["depth", "motion"], [1, 1], "chart.png")
            self.assertIn("| view.only | not run | not run | +15.00 |", table)
            self.assertIn("| total | 41.00 | 100% | +23.00 |", table)
            self.assertIn("Not captured: depth.", table)
            self.assertIn("mean of 1 report windows", table)
            self.assertIn("mean of 1 to 3 report windows",
                          frame_stages.table(order, base, added, ["motion"], [3, 1], "chart.png"))
            (root / "view-motion.log").write_text(
                "ms/frame avg/worst: stage.a 30/31 view.shared 2/2 tail.shared 3/3 motion.tail 1/1 | total 36\n")
            (root / "view-tiles.log").write_text(
                "ms/frame avg/worst: stage.a 40/41 view.shared 1/1 tail.shared 2/2 tiles.tail 1/1 | total 44\n")
            order, base, added = frame_stages.stage_costs(root / "shipped.log", str(root / "view-{view}.log"),
                                                        ["depth", "motion", "tiles"])
            self.assertEqual(order, ["stage.a", "stage.idle", "stage.rest", "view.shared",
                                     "tail.shared", "motion.tail", "tiles.tail"])
            self.assertEqual(list(added), ["motion", "tiles"])
            self.assertEqual(added["motion"]["stage.a"], -10)
            self.assertEqual(added["motion"]["stage.rest"], -1)
            table = frame_stages.table(order, base, added, ["depth", "motion", "tiles"], [1, 1, 1], "chart.png")
            self.assertIn("| stage.a | 40.00 | 98% | -10.00 | +0.00 |", table)
            self.assertIn("| stage.rest | 1.00 | 2% | -1.00 | -1.00 |", table)
            self.assertIn("| view.shared | not run | not run | +2.00 | +1.00 |", table)
            self.assertIn("| total | 41.00 | 100% | -5.00 | +3.00 |", table)
            with self.assertRaises(ValueError):
                frame_stages.stage_costs(root / "missing.log", str(root / "view-{view}.log"), ["motion"])

    def test_each_captured_view_has_its_own_chart(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            shipped = root / "frame.png"
            order = ["stage.a", "view.shared", "stage.idle", "stage.rest"]
            base = {"stage.a": 40, "stage.idle": 0, "stage.rest": 1}
            added = {"motion": {"stage.a": -10, "view.shared": 2, "stage.idle": 0, "stage.rest": -1},
                     "tiles": {"stage.a": 1, "view.shared": 3, "stage.idle": 0, "stage.rest": 0}}
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
                             [["stage.a 98%", "stage.rest 2%"], ["view.shared +5%"], ["stage.a +2%", "view.shared +7%"]])
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
        base = {"stage.a": 7, "stage.b": 2, "stage.rest": 3}
        shipped = sum(base.values())
        shares = frame_stages.stage_shares(base, shipped)
        self.assertAlmostEqual(sum(shares.values()), frame_stages.PERCENT_SCALE)
        self.assertAlmostEqual(shares["stage.a"], 7 * frame_stages.PERCENT_SCALE / shipped)
        added = frame_stages.stage_shares({"stage.a": 3, "stage.rest": -1}, shipped)
        self.assertEqual(added, {"stage.a": 25, "stage.rest": -frame_stages.PERCENT_SCALE / shipped})
        self.assertEqual(frame_stages.share_text(added["stage.a"], signed=True), "+25%")
        self.assertEqual(frame_stages.share_text(added["stage.rest"], signed=True), "-8%")

    def test_segment_labels_and_thin_segment_legends_carry_shares(self):
        base = {"stage.a": 49.5, "stage.b": 0.5}
        added = {"depth": {"stage.a": 20, "stage.b": 0.5}}
        axes = self.chart_axes(base, added)
        self.assertIn("49.5 · 99%", [text.get_text() for text in axes[0].texts])
        self.assertIn("20.0 · +40%", [text.get_text() for text in axes[1].texts])
        self.assertIn("+20.5 ms (+41%)", [text.get_text() for text in axes[1].texts])
        self.assertEqual([[text.get_text() for text in axis.figure.legends[0].texts] for axis in axes],
                         [["stage.a 99%", "stage.b 1%"], ["stage.a +40%", "stage.b +1%"]])
        table = frame_stages.table(list(base), base, added, ["depth"], [1, 1], "chart.png")
        self.assertIn("| Stage | As shipped | Share | depth view adds |", table)
        self.assertIn("| stage.b | 0.50 | 1% | +0.50 |", table)

    def test_zero_rounded_shares_are_absent_from_bars_and_legends(self):
        base = {"stage.a": 99.6, "stage.b": 0.4}
        added = {"motion": {"stage.a": 20, "stage.b": 0.4}}
        axes = self.chart_axes(base, added)
        for axis, value, label in zip(axes, [99.6, 20], ["stage.a 100%", "stage.a +20%"]):
            with self.subTest(chart=axis.get_title()):
                self.assertEqual([bar.get_width() for bar in axis.patches], [value])
                self.assertEqual([text.get_text() for text in axis.figure.legends[0].texts], [label])
        table = frame_stages.table(list(base), base, added, ["motion"], [1, 1], "chart.png")
        self.assertIn("| stage.b | 0.40 | 0% | +0.40 |", table)

    def test_negative_delta_is_absent_from_bars_and_legend(self):
        base = {"stage.a": 90, "stage.rest": 10}
        added = {"motion": {"stage.a": 20, "stage.rest": -5}}
        axis = self.chart_axes(base, added)[-1]
        self.assertEqual([bar.get_width() for bar in axis.patches], [20])
        self.assertEqual([text.get_text() for text in axis.figure.legends[0].texts], ["stage.a +20%"])

    def test_view_legend_handles_match_drawn_bar_styles(self):
        base = {"stage.a": 90, "stage.rest": 10}
        added = {"motion": {"stage.a": 20, "stage.rest": 5}}
        axes = self.chart_axes(base, added)
        for axis in axes:
            handles = axis.figure.legends[0].legend_handles
            self.assertEqual(len(handles), len(axis.patches))
            for handle, bar in zip(handles, axis.patches):
                with self.subTest(chart=axis.get_title(), colour=bar.get_facecolor()):
                    self.assertEqual(handle.get_hatch(), bar.get_hatch())
                    self.assertEqual(handle.get_facecolor(), bar.get_facecolor())
                    self.assertEqual(handle.get_alpha(), bar.get_alpha())

    # One lap costs 100 ns: 1000 starts are 0.1 ms of a lap's time, 10000 are 1 ms.
    LAP_CAPTURE = (
        "I (1) shell: ms/frame avg/worst: stage.first 1/1  stage.outer 5.00/5.2  stage.inner 40.00/41 | total 46\n"
        "I (1) shell: ms/frame laps: lap 100 ns | cpu0 stage.outer.cluster 4.500/1000 stage.outer.wait 0.450/0"
        " stage.inner.set_up 15.000/10000 stage.inner.fill 24.700/0"
        " | cpu1 stage.outer.cluster 4.800/0 stage.inner.set_up 15.500/0\n"
        "I (2) shell: ms/frame avg/worst: stage.first 1/1  stage.outer 5.00/5.2  stage.inner 40.00/41 | total 46\n"
        "I (2) shell: ms/frame laps: lap 100 ns | cpu0 stage.outer.cluster 4.500/1000 stage.outer.wait 0.450/0"
        " stage.inner.set_up 15.000/10000 stage.inner.fill 24.700/0 stage.inner.wait 0.200/0"
        " | cpu1 stage.outer.cluster 4.800/0\n")

    def write_laps(self, root, text=LAP_CAPTURE):
        path = pathlib.Path(root) / "laps.log"
        path.write_text(text)
        return path

    def test_a_lap_missing_from_a_window_counts_as_zero_and_cores_stay_apart(self):
        with tempfile.TemporaryDirectory() as directory:
            stages = frame_stages.sub_stages(self.write_laps(directory))
        total, per_core, untimed = stages["stage.inner"]
        self.assertEqual(total, 40)
        self.assertEqual(list(per_core), ["set_up", "fill", "wait", frame_stages.LAPS_LABEL])
        self.assertAlmostEqual(per_core["set_up"][0], 14.0)
        self.assertAlmostEqual(per_core["set_up"][1], 7.75)
        self.assertEqual(per_core[frame_stages.LAPS_LABEL], {0: 1.0, 1: 0.0})
        self.assertAlmostEqual(per_core["wait"][0], 0.1)
        self.assertAlmostEqual(untimed, 40 - 14 - 24.7 - 0.1 - 1.0)
        self.assertAlmostEqual(stages["stage.outer"][2], 0.05)

    def assert_capture_fails(self, text, message):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(ValueError, message):
                frame_stages.sub_stages(self.write_laps(directory, text))

    def test_laps_leaving_too_much_of_a_parent_untimed_fail(self):
        self.assert_capture_fails(self.LAP_CAPTURE.replace("stage.inner.fill 24.700/0", "stage.inner.fill 20.000/0"),
                                  "stage.inner's 40.00 ms untimed")

    def test_laps_adding_up_past_their_parent_fail_too(self):
        self.assert_capture_fails(self.LAP_CAPTURE.replace("stage.inner.fill 24.700/0", "stage.inner.fill 26.000/0"),
                                  "untimed")

    def test_a_lap_naming_no_bracket_of_the_capture_fails(self):
        self.assert_capture_fails(self.LAP_CAPTURE.replace("stage.inner.fill", "stage.gone.fill"),
                                  "inside no bracket of the capture: stage.gone")

    def test_a_capture_without_laps_fails(self):
        self.assert_capture_fails("ms/frame avg/worst: stage.outer 5/5  stage.inner 40/40 | total 45\n", "no lap windows")

    def test_sub_stage_bars_add_up_to_their_parent_and_keep_a_colour_per_phase(self):
        import matplotlib.pyplot as plt
        from matplotlib.colors import to_rgba
        with tempfile.TemporaryDirectory() as directory, patch("matplotlib.pyplot.close", wraps=plt.close) as close:
            stages = frame_stages.sub_stages(self.write_laps(directory))
            frame_stages.sub_stage_chart(stages, pathlib.Path(directory) / "inside.png")
            axes = close.call_args_list[0].args[0].axes
        for axis, (parent, (total, per_core, untimed)) in zip(axes, stages.items()):
            with self.subTest(parent=parent):
                self.assertEqual(axis.get_title(loc="left"), parent)
                values = [cores[0] for cores in per_core.values()] + [untimed]  # laps' own cost included
                ends = [sum(values[:i + 1]) for i in range(len(values))]
                for bar in axis.patches:  # a segment under half a percent is left out, never shifts the rest
                    self.assertTrue(any(abs(bar.get_x() + bar.get_width() - end) < 1e-9 for end in ends))
                self.assertAlmostEqual(ends[-1], total)
                self.assertIn(f"{total:.1f} ms", [text.get_text() for text in axis.texts])
        colours = [{text.get_text().split()[0]: handle.get_facecolor()
                    for text, handle in zip(axis.get_legend().texts, axis.get_legend().legend_handles)}
                   for axis in axes]
        self.assertNotEqual(colours[1]["set_up"], colours[0]["cluster"])
        self.assertEqual(colours[0]["untimed"], colours[1]["untimed"])
        self.assertEqual(colours[1][frame_stages.LAPS_LABEL][:3], to_rgba(frame_stages.LAPS_COLOUR)[:3])

    def test_the_sub_stages_sit_inline_and_the_debug_views_fold_away(self):
        with tempfile.TemporaryDirectory() as directory:
            stages = frame_stages.sub_stages(self.write_laps(directory))
        inside = frame_stages.sub_stage_table(stages, 2, "frame.sub-stages.png")
        self.assertIn("| stage.inner.set_up | 14.00 | 7.75 | 35% |", inside)
        self.assertIn(f"| stage.inner {frame_stages.LAPS_LABEL} | 1.00 | 0.00 | 2% |", inside)
        self.assertIn("| stage.inner | 40.00 |  | 100% |", inside)
        self.assertIn("mean of 2 lap windows", inside)
        base = {"stage.a": 40.0}
        body = frame_stages.table(["stage.a"], base, {"depth": {"stage.a": 1.0}}, ["depth"], [1, 1], "frame.png",
                                  inside)
        views = body.index("<summary>Debug views")
        self.assertLess(body.index("![Frame stages as shipped]"), body.index("![Inside transform and draw]"))
        self.assertLess(body.index("![Inside transform and draw]"), views)
        self.assertGreater(body.index("![Frame stages with the depth view]"), views)

    def test_view_names_come_from_the_view_table(self):
        names = frame_stages.view_names()
        self.assertEqual(names, ["depth", "tiles", "motion", "meshlets", "cull", "boxes", "overdraw"])
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "context.c"
            source.write_text('static const render_debug_view_t views[] = {\n'
                              '    {"tiles", 0, tiles},\n    {.name = "depth", .attachment = depth},\n};\n'
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
