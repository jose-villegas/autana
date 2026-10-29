"""The tables and figure of docs/render/Frame-Generation.md from
framegen_score.py's CSVs: per scene, render size and look-ahead, the holes
the warp leaves and the reference scorer's numbers for the warp (each hole
fill) against repeating the last frame; and one scene's frames along its
path. With --board, the board's cost table from the framegen_perf suite's
capture: what a generated frame costs and the displayed rate it buys.

    framegen_report.py --out-table TABLE.md --out-plot PLOT.png
        --plot-scene KEY --plot-size WxH [--board LOG --out-board-table TABLE.md]
        CSV [CSV ...]

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
from docs.plot_style import FIGURE_SIZE, SLOTS, figure, save_themed_plot  # noqa: E402
from framegen_score import DISPLAY_STEP_MS  # noqa: E402

# The worst frames are the ones a viewer sees; the mean hides them.
WORST_PERCENTILE = 95
US_PER_MS = 1000
MS_PER_SECOND = 1000
# One framegen_perf line: the scene, the size, then name=value fields (a/b is mean/worst).
BOARD_LINE = re.compile(r"framegen: (?P<scene>\S+) (?P<size>\d+x\d+) (?P<fields>.*)$")


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


def table(groups):
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


def plot(groups, scene, size, path):
    """Mean dE per sampled frame along the path, warp (row fill) and repeat, for each look-ahead."""
    fig = figure(FIGURE_SIZE)
    axis = fig.add_subplot()
    slot = 0
    for (row_scene, row_size, ahead), fills in groups.items():
        if (row_scene, row_size) != (scene, size):
            continue
        for fill, label in (("row", "warped"), ("repeat", "last frame repeated")):
            rows = fills[fill]
            t = column(rows, "frame") * float(DISPLAY_STEP_MS) / 1000
            axis.plot(t, column(rows, "mean_delta_e"), color=SLOTS[slot], label="%s, %d ahead" % (label, ahead))
            slot += 1
    axis.set_ylim(bottom=0)
    axis.set_xlabel("time along the camera path (s)")
    axis.set_ylabel("mean dE76 against the true frame")
    save_themed_plot(fig, axis, path, "%s at %s: generated frame against the true one" % (scene, size))


def read_board(path):
    """{(scene, size): {field: mean}} from a framegen_perf capture; a repeated
    row keeps its last line."""
    rows = collections.OrderedDict()
    for line in pathlib.Path(path).read_text(encoding="utf-8").splitlines():
        match = BOARD_LINE.search(line)
        if match is None:
            continue
        fields = dict(pair.split("=", 1) for pair in match["fields"].split())
        rows[(match["scene"], match["size"])] = {name: float(value.split("/")[0]) for name, value in fields.items()}
    return rows


def board_table(rows):
    """Per scene and size: the rendered frame, a generated frame's parts and
    sum, and the displayed interval with one generated frame per rendered one."""
    lines = ["| Scene | Size | Rendered frame | Warp, both cores (one) | Fill | Present | Generated frame |"
             " Displayed: render only | Displayed: + 1 generated |",
             "|---|---|---|---|---|---|---|---|---|"]
    for (scene, size), f in rows.items():
        # The first capture timed the warp on one core too (warp1_us); later ones only on both (warp_us).
        warp_both = f.get("warp2_us", f.get("warp_us")) / US_PER_MS
        one_core = "%.1f" % (f["warp1_us"] / US_PER_MS) if "warp1_us" in f else "-"
        frame = f["frame_us"] / US_PER_MS
        fill, present = f["fill_us"] / US_PER_MS, f["present_us"] / US_PER_MS
        generated = warp_both + fill + present
        paired = (frame + generated) / 2
        lines.append("| %s | %s | %.1f ms | %.1f ms (%s) | %.1f ms | %.1f ms | %.1f ms | %.0f fps | %.0f fps |" % (
            scene, size, frame, warp_both, one_core, fill, present, generated,
            MS_PER_SECOND / frame, MS_PER_SECOND / paired))
    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--out-table", required=True)
    parser.add_argument("--out-plot", required=True)
    parser.add_argument("--plot-scene", required=True)
    parser.add_argument("--plot-size", required=True)
    parser.add_argument("--board")
    parser.add_argument("--out-board-table")
    parser.add_argument("csv", nargs="+")
    args = parser.parse_args()
    groups = grouped(read_rows(args.csv))
    pathlib.Path(args.out_table).write_text(table(groups), newline="\n")
    plot(groups, args.plot_scene, args.plot_size, args.out_plot)
    if args.board:
        pathlib.Path(args.out_board_table).write_text(board_table(read_board(args.board)), newline="\n")


if __name__ == "__main__":
    main()
