"""The tables and figures of docs/render/Frame-Generation.md.

From framegen_score.py's CSVs: per scene, render size and look-ahead, the
holes the warp leaves and the reference scorer's numbers for the warp (each
hole fill) against repeating the last frame, and one scene's frames along its
path. From the board captures (the render lab's framegen_perf and
framegen_modes suites): what a generated frame costs, where the warp's time
goes, each scene played in every mode, and the pricing rule drawn over the
measured generated frames.

    framegen_report.py --tables DIR --images DIR --plot-scene KEY --plot-size WxH
        --board LOG [--board LOG ...] CSV [CSV ...]

Needs matplotlib, Pillow and numpy (tools/r3d/requirements.txt).
"""
import argparse
import collections
import csv
import pathlib
import re
import sys

import numpy as np

TOOLS = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(TOOLS))
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from build.c_header import read_define  # noqa: E402
from docs.plot_style import FIGURE_SIZE, SLOTS, figure, save_themed_plot  # noqa: E402
from framegen_score import DISPLAY_STEP_MS  # noqa: E402

FRAMEGEN_HEADER = TOOLS.parent / "main/render/framegen/framegen.h"
SHOWN_PER_DRAWN = read_define(FRAMEGEN_HEADER, "FRAMEGEN_SHOWN_PER_DRAWN")
# The worst frames are the ones a viewer sees; the mean hides them.
WORST_PERCENTILE = 95
US_PER_MS = 1000
MS_PER_SECOND = 1000
# A board line: its kind, the scene, an optional size, then name=value fields (a/b is mean/worst).
BOARD_LINE = re.compile(r"(?P<kind>framegen|framegen_profile|framegen_modes): (?P<scene>\S+)"
                        r"(?: (?P<size>\d+x\d+))? (?P<fields>\S+=.*)$")
# The pricing figure: generated frame costs up to this share of the shown frame's budget.
PRICING_SPAN = 1.5
PRICING_SAMPLES = 100


def read_rows(paths):
    rows = []
    for path in paths:
        with open(path, newline="") as f:
            rows.extend(csv.DictReader(f))
    return rows


def grouped(rows):
    """{(scene, size, ahead): {fill: [rows]}} in the order first seen."""
    groups = collections.OrderedDict()
    for row in rows:
        key = (row["scene"], "%sx%s" % (row["width"], row["height"]), int(row["ahead"]))
        groups.setdefault(key, collections.defaultdict(list))[row["fill"]].append(row)
    return groups


def column(rows, name):
    return np.array([float(row[name]) for row in rows])


def scores_table(groups):
    lines = ["| Scene | Size | Ahead | Holes %% (pinholes) | dE warp, row fill | dE warp, previous fill | dE repeat |"
             " p%d frame dE warp / repeat | SSIM warp / repeat |" % WORST_PERCENTILE,
             "|---|---|---|---|---|---|---|---|---|"]
    for (scene, size, ahead), fills in groups.items():
        warp, previous, repeat = fills["row"], fills["previous"], fills["repeat"]
        worst = lambda rows: np.percentile(column(rows, "mean_delta_e"), WORST_PERCENTILE)  # noqa: E731
        lines.append("| %s | %s | %d | %.1f (%.1f) | %.2f | %.2f | %.2f | %.2f / %.2f | %.3f / %.3f |" % (
            scene, size, ahead, column(warp, "holes_pct").mean(), column(warp, "pinholes_pct").mean(),
            column(warp, "mean_delta_e").mean(), column(previous, "mean_delta_e").mean(),
            column(repeat, "mean_delta_e").mean(), worst(warp), worst(repeat), column(warp, "ssim").mean(),
            column(repeat, "ssim").mean()))
    return "\n".join(lines) + "\n"


def path_plot(groups, scene, size, path):
    """Mean dE per sampled frame along the path, warp (row fill) and repeat, for each look-ahead."""
    fig = figure(FIGURE_SIZE)
    axis = fig.add_subplot()
    slot = 0
    for (row_scene, row_size, ahead), fills in groups.items():
        if (row_scene, row_size) != (scene, size):
            continue
        for fill, label in (("row", "warped"), ("repeat", "last frame repeated")):
            rows = fills[fill]
            t = column(rows, "frame") * float(DISPLAY_STEP_MS) / MS_PER_SECOND
            axis.plot(t, column(rows, "mean_delta_e"), color=SLOTS[slot], label="%s, %d ahead" % (label, ahead))
            slot += 1
    axis.set_ylim(bottom=0)
    axis.set_xlabel("time along the camera path (s)")
    axis.set_ylabel("mean dE76 against the true frame")
    save_themed_plot(fig, axis, path, "%s at %s: generated frame against the true one" % (scene, size))


def read_board(paths):
    """{kind: {(scene, size or mode): {field: mean}}} from board captures; a
    repeated row keeps its last line, so a later capture replaces an earlier one."""
    rows = collections.defaultdict(collections.OrderedDict)
    for path in paths:
        for line in pathlib.Path(path).read_text(encoding="utf-8").splitlines():
            match = BOARD_LINE.search(line)
            if match is None:
                continue
            fields = {name: float(value.split("/")[0])
                      for name, value in (pair.split("=", 1) for pair in match["fields"].split())}
            key = match["size"] or (fields["framegen"], fields["budget_ms"])
            rows[match["kind"]][(match["scene"], key)] = fields
    return rows


def generated_ms(f):
    """A generated frame's cost: the warp on both cores, the fill and its compose."""
    # The first capture timed the warp on one core too (warp1_us) and called both cores warp2_us.
    return (f.get("warp2_us", f.get("warp_us")) + f["fill_us"] + f["present_us"]) / US_PER_MS


def cost_table(rows):
    """Per scene and size: the drawn frame and a generated frame's parts and sum."""
    lines = ["| Scene | Size | Drawn frame | Warp | Fill | Compose | Generated frame |",
             "|---|---|---|---|---|---|---|"]
    for (scene, size), f in rows.items():
        lines.append("| %s | %s | %.1f ms | %.1f ms | %.1f ms | %.1f ms | %.1f ms |" % (
            scene, size, f["frame_us"] / US_PER_MS, f.get("warp2_us", f.get("warp_us")) / US_PER_MS,
            f["fill_us"] / US_PER_MS, f["present_us"] / US_PER_MS, generated_ms(f)))
    return "\n".join(lines) + "\n"


def profile_table(rows):
    """The warp timed four ways: the target's clear alone, every splat off
    screen (clear, maths and source reads), to the drawn view (adds the writes
    in order) and to the next view (adds their scatter)."""
    lines = ["| Scene | Size | Clear | Off screen | To the drawn view | To the next view |",
             "|---|---|---|---|---|---|"]
    for (scene, size), f in rows.items():
        lines.append("| %s | %s | %.1f ms | %.1f ms | %.1f ms | %.1f ms |" % (
            scene, size, f["clear_us"] / US_PER_MS, f["offscreen_us"] / US_PER_MS, f["same_view_us"] / US_PER_MS,
            f["warp_us"] / US_PER_MS))
    return "\n".join(lines) + "\n"


def modes_table(rows):
    """Each scene's path played in real time per mode: frames shown per second,
    the slowest twentieth, how many were drawn, and the drawn height."""
    lines = ["| Scene | Frame generation | Budget | Shown | p%d frame | Drawn | Drawn height |" % WORST_PERCENTILE,
             "|---|---|---|---|---|---|---|"]
    for (scene, _), f in rows.items():
        budget = "off" if f["budget_ms"] == 0 else "%d ms" % f["budget_ms"]
        lines.append("| %s | %s | %s | %.1f fps | %.1f ms | %d%% | %d |" % (
            scene, "on" if f["framegen"] else "off", budget, MS_PER_SECOND * US_PER_MS / f["mean_us"],
            f["p95_us"] / US_PER_MS, round(100 * f["drawn"] / f["frames"]), f["mean_height"]))
    return "\n".join(lines) + "\n"


def drawn_budget(shown_ms, generated):
    """The pricing rule: what a drawn frame may spend, never less than one shown frame's budget."""
    return np.maximum(SHOWN_PER_DRAWN * shown_ms - (SHOWN_PER_DRAWN - 1) * generated, shown_ms)


def pricing_plot(rows, scene, budgets_ms, path):
    """The drawn frame's budget against the generated frame's cost, per shown
    budget, with `scene`'s measured generated frames marked."""
    fig = figure(FIGURE_SIZE)
    axis = fig.add_subplot()
    for slot, shown_ms in enumerate(budgets_ms):
        g = np.linspace(0, PRICING_SPAN * shown_ms, PRICING_SAMPLES)
        axis.plot(g, drawn_budget(shown_ms, g), color=SLOTS[slot], label="budget %d ms per shown frame" % shown_ms)
    for (row_scene, size), f in rows.items():
        if row_scene != scene:
            continue
        g = generated_ms(f)
        axis.axvline(g, color=SLOTS[len(budgets_ms)], linewidth=0.8)
        axis.annotate(size, (g, 0), rotation=90, va="bottom", ha="right", fontsize="x-small")
    axis.set_ylim(bottom=0)
    axis.set_xlim(left=0)
    axis.set_xlabel("generated frame cost (ms)")
    axis.set_ylabel("budget of each drawn frame (ms)")
    save_themed_plot(fig, axis, path, "a drawn frame gets what its generated frame leaves")


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--tables", type=pathlib.Path, required=True)
    parser.add_argument("--images", type=pathlib.Path, required=True)
    parser.add_argument("--plot-scene", required=True)
    parser.add_argument("--plot-size", required=True)
    parser.add_argument("--board", action="append", default=[])
    parser.add_argument("csv", nargs="+")
    args = parser.parse_args()
    groups = grouped(read_rows(args.csv))
    board = read_board(args.board)
    tables = {"framegen-scores": scores_table(groups), "framegen-board": cost_table(board["framegen"]),
              "framegen-profile": profile_table(board["framegen_profile"]),
              "framegen-modes": modes_table(board["framegen_modes"])}
    for name, text in tables.items():
        (args.tables / (name + ".md")).write_text(text, newline="\n")
    path_plot(groups, args.plot_scene, args.plot_size, args.images / "framegen-bistro-path.png")
    budgets = sorted({int(f["budget_ms"]) for f in board["framegen_modes"].values() if f["budget_ms"]}, reverse=True)
    pricing_plot(board["framegen"], args.plot_scene, budgets, args.images / "framegen-pricing.png")


if __name__ == "__main__":
    main()
