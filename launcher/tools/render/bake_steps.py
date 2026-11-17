#!/usr/bin/env python3
"""Each step of a scene's mesh bakes: its wall time, the triangles it took and
left, and the memory it peaked at. Run on a scene, it times the bakes on this
machine; the doc-images GPU stage (doc_stages.py) uses the same rows for
docs/render/offline/README.md's `bake-steps` chart and table.

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
from render.byte_sizes import GB_BYTES, MB_BYTES
from render.doc_tables import collapsed, markdown

PERCENT = 100
OTHER_STEPS = "other steps"  # the steps under MIN_SHARE in every bake, drawn as one segment
MIN_SHARE = 1  # percent: a step this share of some bake or more gets its own colour
PANEL_WIDTH = 6  # inches
ROW_HEIGHT = 0.32  # inches per bar
PANEL_ROOM = 1.2  # inches for each panel's title, axis and legend
TRIANGLES = "triangles"
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
        return "not available" if value is None else f"{value / MB_BYTES:.1f}"
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
    parts = [f"{value / GB_BYTES:.1f} GiB of {name} (in {step})"
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
    """Top: each CPU bake's steps as shares of its wall time. Bottom: the triangles each step leaves."""
    from docs.plot_style import LABEL_SIZE, MUTED, SLOTS, save, stacked_barh, style_axes, subplots
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
    figure, (top, bottom) = subplots(2, 1, (PANEL_WIDTH, ROW_HEIGHT * (len(cpu) + len(bars)) + 2 * PANEL_ROOM),
                                     gridspec_kw=dict(height_ratios=[len(cpu), len(bars)]))
    stacked = []
    for variant, rows in cpu:
        merged = {}
        for row, share in zip(rows, shares(rows)):
            step = row["step"] if row["step"] in named else OTHER_STEPS
            merged[step] = merged.get(step, 0.0) + share
        stacked.append((variant, list(merged.items())))
    stacked_barh(top, stacked, colour, extent=PERCENT)
    handles = dict(zip(*reversed(top.get_legend_handles_labels())))
    figure.legend([handles[step] for step in colour if step in handles], [step for step in colour if step in handles],
                  loc="outside upper center", ncol=LEGEND_COLUMNS, frameon=False, fontsize=LABEL_SIZE)
    top.set_xlabel("share of the bake's wall time, %", fontsize=LABEL_SIZE, color=MUTED)
    style_axes(top, "Where each CPU bake spends its time")
    stacked_barh(bottom, [(label, [(TRIANGLES, count)]) for label, count in bars], {TRIANGLES: SLOTS[0]},
                 [f"{count:,}" for _, count in bars])
    bottom.xaxis.set_major_formatter(lambda value, _: f"{value / THOUSAND:g}k")
    bottom.set_xlabel(TRIANGLES, fontsize=LABEL_SIZE, color=MUTED)
    style_axes(bottom, "Triangles each step leaves")
    save(figure, path)


def block(variants, chart_path):
    """The `bake-steps` block: the chart at the doc-relative `chart_path`, the memory the bakes
    need, then the rows collapsed."""
    return (f"![Bake steps]({chart_path})\n\n{memory_line(variants)}\n\n"
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
