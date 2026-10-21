#!/usr/bin/env python3
"""Each step of a scene's mesh bakes: its wall time, the triangles it took and
left, and the memory it peaked at. Run on a scene, it times the bakes on this
machine; the doc-images GPU stage (doc_stages.py) uses the same rows for
Render-Pipeline.md's `bake-steps` chart and table.

    python launcher/tools/render/bake_steps.py SCENE.scene.toml [--mesh NAME]

The meshes are baked into a scratch folder and thrown away; a fitted variant
is skipped, as its fit needs a CUDA GPU.
"""

import argparse
import pathlib
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "launcher/tools"))
from render.doc_tables import collapsed, markdown

PERCENT = 100
BYTES_PER_MIB = 1024 ** 2
BYTES_PER_GIB = 1024 ** 3
OTHER_STEPS = "other steps"  # the steps under MIN_SHARE in every bake, drawn as one segment
MIN_SHARE = 1  # percent: a step this share of some bake or more gets its own colour
PANEL_WIDTH = 6  # inches
ROW_HEIGHT = 0.32  # inches per bar
PANEL_ROOM = 1.2  # inches for each panel's title, axis and legend
BAR_HEIGHT = 0.7
LABEL_ROOM = 0.3  # share of the widest bar left free for its label
LEGEND_COLUMNS = 4
THOUSAND = 1000


def on_gpu(rows):
    """Whether a run is a GPU one: only GPU steps read VRAM."""
    return any(row["peak_vram_bytes"] is not None for row in rows)


def shares(rows):
    total = sum(row["wall_s"] for row in rows)
    return [PERCENT * row["wall_s"] / total if total else 0.0 for row in rows]


def steps_table(variants, seconds=False):
    """One row per step of each (variant, rows) run; `seconds` adds the wall time, which only the machine
    that ran the bake can read anything into."""
    def mib(value):
        return "not available" if value is None else f"{value / BYTES_PER_MIB:.1f}"
    headers = ["Variant", "Step", *(["Wall s"] if seconds else []), "Share of variant", "Triangles in",
               "Triangles out", "Peak RAM MiB", "Peak VRAM MiB"]
    return markdown(headers, [(variant, row["step"], *([f'{row["wall_s"]:.3f}'] if seconds else []), f"{share:.0f}%",
                               row["triangles_in"], row["triangles_out"], mib(row["peak_ram_bytes"]),
                               mib(row["peak_vram_bytes"]))
                              for variant, rows in variants for row, share in zip(rows, shares(rows))])


def memory_line(variants):
    """The peak RAM and VRAM a run needed, with the step that needed it."""
    def peak(key):
        found = [(row[key], row["step"]) for _, rows in variants for row in rows if row[key] is not None]
        return max(found) if found else None
    parts = [f"{value / BYTES_PER_GIB:.1f} GiB of {name} (in {step})"
             for name, found in (("RAM", peak("peak_ram_bytes")), ("VRAM", peak("peak_vram_bytes"))) if found
             for value, step in [found]]
    return "Peak memory: " + " and ".join(parts) + "."


def triangle_bars(variants):
    """(label, triangles) after each step that changes the count: once when every run leaves the same count,
    else once per run."""
    counts = {}
    for variant, rows in variants:
        for row in rows:
            if row["triangles_out"] != row["triangles_in"]:
                counts.setdefault(row["step"], []).append((variant, row["triangles_out"]))
    bars = []
    for step, found in counts.items():
        if len({count for _, count in found}) == 1:
            bars.append((f"after {step}", found[0][1]))
        else:
            bars.extend((f"after {step}, {variant}", count) for variant, count in found)
    return bars


def chart(variants, path):
    """Left: each CPU bake's steps as shares of its wall time. Right: the triangles each step leaves."""
    from docs.plot_style import INK, LABEL_SIZE, MUTED, SLOTS, save, style_axes, subplots
    cpu = [(variant, rows) for variant, rows in variants if not on_gpu(rows)]
    named = []
    for _, rows in cpu:
        for row, share in zip(rows, shares(rows)):
            if share >= MIN_SHARE and row["step"] not in named:
                named.append(row["step"])
    if len(named) >= len(SLOTS):
        raise ValueError(f"{len(named)} steps, {len(SLOTS) - 1} colours")
    colour = dict(zip([*named, OTHER_STEPS], SLOTS))
    bars = triangle_bars(variants)
    figure, (left, right) = subplots(2, 1, (PANEL_WIDTH, ROW_HEIGHT * (len(cpu) + len(bars)) + 2 * PANEL_ROOM),
                                     gridspec_kw=dict(height_ratios=[len(cpu), len(bars)]))
    for index, (_, rows) in enumerate(cpu):
        start = 0.0
        merged = {}
        for row, share in zip(rows, shares(rows)):
            step = row["step"] if row["step"] in named else OTHER_STEPS
            merged[step] = merged.get(step, 0.0) + share
        for step, share in merged.items():
            left.barh(index, share, left=start, color=colour[step], height=BAR_HEIGHT, edgecolor="white",
                      label=step)
            start += share
    handles, labels = left.get_legend_handles_labels()
    unique = dict(zip(labels, handles))
    figure.legend([unique[step] for step in colour if step in unique], [step for step in colour if step in unique],
                loc="outside upper center", ncol=LEGEND_COLUMNS, frameon=False, fontsize=LABEL_SIZE)
    left.set_yticks(range(len(cpu)), [variant for variant, _ in cpu])
    left.set_xlim(0, PERCENT)
    left.set_xlabel("share of the bake's wall time, %", fontsize=LABEL_SIZE, color=MUTED)
    style_axes(left, "Where each CPU bake spends its time")
    for index, (label, count) in enumerate(bars):
        right.barh(index, count, color=SLOTS[0], height=BAR_HEIGHT)
        right.annotate(f"{count:,}", (count, index), xytext=(4, 0), textcoords="offset points", va="center",
                       fontsize=LABEL_SIZE, color=INK)
    right.set_yticks(range(len(bars)), [label for label, _ in bars])
    right.set_xlim(0, max(count for _, count in bars) * (1 + LABEL_ROOM))
    right.xaxis.set_major_formatter(lambda value, _: f"{value / THOUSAND:g}k")
    right.set_xlabel("triangles", fontsize=LABEL_SIZE, color=MUTED)
    style_axes(right, "Triangles each step leaves")
    for axis in (left, right):
        axis.invert_yaxis()
        axis.tick_params(axis="y", labelsize=LABEL_SIZE)
        for side in ("top", "right"):
            axis.spines[side].set_visible(False)
    save(figure, path)


def block(variants, chart_name):
    """Render-Pipeline.md's `bake-steps` block: the chart, the memory the bakes need, then the rows collapsed."""
    return (f"![Bake steps](../images/render/gpu/{chart_name})\n\n{memory_line(variants)}\n\n"
            + collapsed("Bake steps", steps_table(variants)))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("scene", type=pathlib.Path, help="a .scene.toml file")
    parser.add_argument("--mesh", help="time only the mesh with this name")
    args = parser.parse_args(argv)
    from r3d.import_settings import SettingsError, load_scene
    from r3d.mesh_import import write_baked
    from r3d.process_budget import StepRecorder
    try:
        scene = load_scene(args.scene.resolve())
    except SettingsError as error:
        parser.error(str(error))
    jobs = [job for job in scene.renderers if not job.renderer.fit
            and args.mesh in (None, job.renderer.variant.name, job.asset_name)]
    if not jobs:
        parser.error(f"no CPU-baked mesh named {args.mesh!r}" if args.mesh else "the scene has no CPU-baked mesh")
    variants = []
    with tempfile.TemporaryDirectory() as directory:
        for job in jobs:
            recorder = StepRecorder()
            try:
                write_baked(job, scene, pathlib.Path(directory), job.asset_name, recorder=recorder)
            except SettingsError as error:
                parser.error(str(error))
            variants.append((job.asset_name, recorder.rows))
    print(steps_table(variants, seconds=True), end="")
    print(memory_line(variants))
    return 0


if __name__ == "__main__":
    sys.exit(main())
