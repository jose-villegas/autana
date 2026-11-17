"""Generate the board table from committed suite result lines without measuring the host."""
import argparse
import pathlib
import re
import statistics
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "render"))
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "perf"))
from capture import build_ids
from generated_blocks import replace_blocks

MODES = ("direct", "renormalised", "table")
# The columns that leave the table for one sentence when every row holds the same value.
SHARED = (("Normal-palette build cycles / frame", "the normal palette costs {} cycles to build per frame"),
          ("Renderer internal bytes", "the renderer holds {} bytes of internal memory"),
          ("Scene PSRAM bytes", "the scene holds {} bytes of PSRAM"))
# Point-label offsets in points from the point, tried in order until one clears the labels already placed.
LABEL_OFFSETS = ((5, 4), (5, -10), (-12, 4), (-12, -10), (5, 12), (5, -18), (-12, 12), (-12, -18))
PLOT_SIZE = (7.0, 3.2)  # inches
PLOT_MARGIN = 0.08  # of the cost range, either side
FIGURE = pathlib.Path(__file__).resolve().parents[4] / "docs/render/images/skin-light-board.png"


def samples(text):
    build = label = ""
    vertices = 0
    pending = None
    for line in text.splitlines():
        if build_ids(line):
            if pending:
                yield pending
                pending = None
            build = build_ids(line)[0]
        match = re.search(r"vertices=(\d+)", line)
        if match:
            vertices = int(match[1])
        match = re.search(r"skin_light_perf: (test_\w+) .*med=\d+cycles", line)
        if match:
            if pending:
                yield pending
                pending = None
            label = match[1].removeprefix("test_")
        match = re.search(r"COST cycles_per_(vertex|build)=([\d.]+)", line)
        if match:
            cost = float(match[2])
            pending = [build, label, f"{cost:.3f}", match[1],
                       f"{cost / vertices:.3f}" if match[1] == "build" else "—", "—", "—"]
        match = re.search(r"QUALITY max=(\d+) mean=([\d.]+)", line)
        if match and pending:
            pending[-2:] = match.groups()
    if pending:
        yield pending


def rows(text):
    grouped = {}
    for row in samples(text):
        grouped.setdefault((row[0], row[1], row[3]), []).append(row)
    for runs in grouped.values():
        row = runs[0].copy()
        for column in (2, 4):
            if row[column] != "—":
                row[column] = f"{statistics.median(float(run[column]) for run in runs):.3f}"
        quality = [run for run in runs if run[-2] != "—"]
        if quality:
            row[-2] = str(max(int(run[-2]) for run in quality))
            row[-1] = max(quality, key=lambda run: float(run[-1]))[-1]
        yield row


def mode_points(text):
    """The latest runtime-table build's points: ([(mode, lights, cycles, mean error, worst error, table build,
    scene internal bytes)], skinning-alone cycles per vertex, {shared column: value per row}, build id)."""
    measured = list(rows(text))
    builds = [row[0] for row in measured if re.fullmatch(r"fused_table_\d+", row[1])]
    if not builds:
        raise ValueError("no runtime table capture")
    build = builds[-1]
    costs = {row[1]: row for row in measured if row[0] == build}
    memory = {}
    current_build = ""
    for line in text.splitlines():
        if build_ids(line):
            current_build = build_ids(line)[0]
        match = re.search(r"RAM mode=(\w+) renderer_bytes=(\d+) scene_bytes=(\d+).*scene_metadata_psram=(\d+)", line)
        if match and current_build == build:
            memory[match[1]] = (match[2], match[3], match[4])
    if not any(re.fullmatch(r"table_build_\d+", name) for name in costs):
        raise ValueError("no runtime table build capture")
    palette = costs["normal_palette_build"][2] if "normal_palette_build" in costs else "—"
    points, shared = [], {name: [] for name, _ in SHARED}
    for mode in MODES:
        for lights in sorted(int(match[1]) for name in costs
                             if (match := re.fullmatch(rf"fused_{mode}_(\d+)", name))):
            row = costs[f"fused_{mode}_{lights}"]
            build_row = costs.get(f"table_build_{lights}") if mode == "table" else None
            renderer, scene, psram = memory.get(mode, ("—", "—", "—"))
            points.append((mode, lights, float(row[2]), float(row[-1]), row[-2], build_row[2] if build_row else "—", scene))
            for (name, _), value in zip(SHARED, (palette, renderer, psram)):
                shared[name].append(value)
    skin = float(costs["skin_only"][2]) if "skin_only" in costs else None
    return points, skin, shared, build


def lighting_modes(text):
    """The slim table of the modes, preceded by one sentence for each column that holds a single value in every row;
    a column whose rows differ stays in the table."""
    points, skin, shared, _ = mode_points(text)
    constant = {name: set(values).pop() for name, values in shared.items() if len(set(values)) == 1}
    names = [name for name, _ in SHARED if name in constant]
    columns = ["Mode", "Lights", "Worst byte error", "Table build cycles / light change", "Scene internal bytes"]
    columns += [name for name, _ in SHARED if name not in constant]
    table = ["| " + " | ".join(columns) + " |", "|---|---:" + "|---:" * (len(columns) - 2) + "|"]
    for index, (mode, lights, _, _, worst, table_build, scene) in enumerate(points):
        extra = [shared[name][index] for name, _ in SHARED if name not in constant]
        cells = [mode, str(lights), worst, table_build.removesuffix(".000"), scene, *(v.removesuffix(".000") for v in extra)]
        table.append("| " + " | ".join(cells) + " |")
    sentence = ""
    if names:
        parts = [dict(SHARED)[name].format(constant[name].removesuffix(".000")) for name in names]
        sentence = "In every row " + ", ".join(parts[:-1]) + (" and " if len(parts) > 1 else "") + parts[-1] + ".\n\n"
    lead = f"Skinning alone costs {skin:.0f} cycles per vertex.\n\n" if skin is not None else ""
    return lead + sentence + "\n".join(table)


def write_figure(text, output):
    """Mean byte error against cycles per vertex, one colour per mode, the light counts joined and labelled."""
    sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2]))
    from docs.plot_style import FAINT, INK, LABEL_SIZE, MUTED, SLOTS, TITLE_SIZE, save, subplots
    points, _, _, _ = mode_points(text)
    plot, axis = subplots(1, 1, PLOT_SIZE)
    placed = []
    for colour, mode in zip(SLOTS, MODES):
        own = [p for p in points if p[0] == mode]
        axis.plot([p[2] for p in own], [p[3] for p in own], "o-", color=colour, label=mode, markersize=4, linewidth=1.4)
    plot.canvas.draw()
    renderer = plot.canvas.get_renderer()
    for colour, mode in zip(SLOTS, MODES):
        for _, lights, cycles, error, *_ in (p for p in points if p[0] == mode):
            for dx, dy in LABEL_OFFSETS:
                label = axis.annotate(f"{lights}", (cycles, error), xytext=(dx, dy), textcoords="offset points",
                                      fontsize=LABEL_SIZE, color=colour)
                box = label.get_window_extent(renderer).expanded(1.1, 1.1)
                if not any(box.overlaps(other) for other in placed):
                    placed.append(box)
                    break
                label.remove()
    axis.set_ylim(0, 1.1 * max(p[3] for p in points))
    axis.margins(x=PLOT_MARGIN)
    axis.set_title("Lighting modes: error against cost, labelled by light count", fontsize=TITLE_SIZE, color=INK)
    axis.set_xlabel("cycles per vertex", fontsize=LABEL_SIZE, color=MUTED)
    axis.set_ylabel("mean byte error", fontsize=LABEL_SIZE, color=MUTED)
    axis.tick_params(labelsize=LABEL_SIZE, colors=MUTED)
    for spine in axis.spines.values():
        spine.set_color(FAINT)
    axis.legend(fontsize=LABEL_SIZE, frameon=False, loc="upper left")
    save(plot, output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=pathlib.Path)
    parser.add_argument("document", type=pathlib.Path)
    parser.add_argument("--figure", type=pathlib.Path, default=FIGURE, help="the PNG to write; needs matplotlib")
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    capture = args.capture.read_text(encoding="utf-8")
    try:
        modes = lighting_modes(capture)
    except ValueError as error:
        parser.error(str(error))
    table = ["| Build id | Kernel | Cycles | Per | Build / vertex | Max byte error | Mean byte error |",
             "|---|---|---:|---|---:|---:|---:|"]
    table.extend("| " + " | ".join(row) + " |" for row in rows(capture))
    command = ("python launcher/tools/r3d/skin_light/report_skin_light_board.py "
               "docs/render/data/skin-light-board.txt docs/render/animation/Skinned-Lighting.md --check")
    changed = replace_blocks(args.document, {"skin-light-board": "\n".join(table), "skin-light-modes": modes},
                             check=args.check, command=command)
    if not args.check:
        write_figure(capture, args.figure)
    return int(args.check and changed)


if __name__ == "__main__":
    sys.exit(main())
