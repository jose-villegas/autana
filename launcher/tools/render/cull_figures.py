#!/usr/bin/env python3
"""What the cull keeps along a scene's camera path, seen and counted, from the
engine's own `cull` view (render/view/raster_cull_view.c) on the host viewer.
launcher/tools/render/render_doc_images.sh runs it once per scene.

    python launcher/tools/render/cull_figures.py --scene SCENE.scene.toml --object NAME --camera NAME
        --stem STEM --label TEXT --level N --out DIR --tables DIR --work DIR [--overdraw STEM]

It writes, into --out:
  STEM-sheet.png   a row per pose, at the panel's size: the camera's picture, the fates from above
                   and from behind, and the tree's level-N node boxes from above
  STEM-counts.png  the counts along the path, clusters and triangles
  OVERDRAW-sheet.png, with --overdraw OVERDRAW: the same poses in landscape at the scene's own render
                   size, the camera's picture beside the `overdraw` view (render/view/raster_overdraw.c)
and leaves --work/STEM-chase.avi (the fates from behind, the whole path) for a
GIF, and --tables/STEM.md, the counts' path means for the page's block.
The counts' series come from cull_counts.py.
"""

import argparse
import csv
import pathlib
import re
import subprocess
import sys

from PIL import Image

ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "launcher/tools"))
sys.path.insert(0, str(ROOT / "launcher/tools/render"))
import check_avi
from build.c_header import read_define
from cull_counts import CULL_HEADER, plot_counts
from docs.plot_style import save, subplots
from render.doc_tables import markdown
from render_compare import still_sheet

VIEWER = ROOT / "launcher/tools/render/scene_viewer.sh"
GFX_HEADER = ROOT / "launcher/main/gfx/gfx.h"
DISPLAY_HEADER = ROOT / "launcher/main/display/display.h"
FRAMES = 120  # frames along the whole path, for the counts and the videos
POSES = 3  # sheet rows, evenly inside the path
FIGURE_SIZE = (10, 5.2)
PERCENT = 100

def enum_value(enum, prefix, name, source=CULL_HEADER):
    """A cullview tunable's value: `name`'s place in raster_cull_view.h's `enum`."""
    text = pathlib.Path(source).read_text(encoding="utf-8")
    body = re.search(r"typedef enum(?: __attribute__\(\(\w+\)\))? \{(.*?)\} " + enum + ";", text, re.S)[1]
    return re.findall(prefix + r"(\w+)", body).index(name)


def cull_from(name):
    return enum_value("raster_cull_from_t", "RASTER_CULL_FROM_", name)


def viewer(args, *flags, log):
    """Runs the scene viewer on the scene; its stderr goes to `log`."""
    command = ["sh", str(VIEWER), str(args.scene), "--object", args.object, "--camera", args.camera, *flags]
    with open(log, "w") as handle:
        subprocess.run(command, check=True, stdout=handle, stderr=subprocess.STDOUT)
    return pathlib.Path(log).read_text(encoding="utf-8", errors="replace")


def path_ms(args):
    text = viewer(args, "--frames", "1", "--dt", "16", "-o", str(args.work / "probe.bmp"),
                  log=args.work / f"{args.stem}-probe.log")
    return int(re.search(r"camera path (\d+) ms", text)[1])


def cull(*settings):
    """--view cull with cullview tunables set, from (name, value) pairs."""
    flags = ["--view", "cull"]
    for name, value in settings:
        flags += ["--set", f"cullview.{name}={value}"]
    return flags


def panel_size(source=GFX_HEADER):
    """The panel's size, WxH as gfx.h defines it: the figures render at it,
    above the half size the board draws, so their borders are not blocky."""
    text = pathlib.Path(source).read_text(encoding="utf-8")
    width, height = (re.search(rf"#define GFX_{axis}\s+(\d+)", text)[1] for axis in ("WIDTH", "HEIGHT"))
    return f"{width}x{height}"


def video(args, name, view_flags, dt, size=None):
    avi = args.work / f"{args.stem}-{name}.avi"
    viewer(args, *view_flags, *(size or ("--size", panel_size())), "--frames", str(FRAMES), "--dt", str(dt), "-o", str(args.work / f"{name}.bmp"),
           "--video", str(avi), log=args.work / f"{args.stem}-{name}.log")
    return avi


def frames_at(avi, indices):
    """The frames at `indices` as RGB images (AVI frames are bottom-up BGR)."""
    movie = check_avi.read_avi(avi)
    wanted = set(indices)
    found = {}
    for index, frame in enumerate(movie.frames):
        if index in wanted:
            stride = (movie.width * 3 + 3) // 4 * 4
            picture = Image.frombuffer("RGB", (movie.width, movie.height), bytes(frame), "raw", "BGR", stride, -1)
            found[index] = picture.copy()
    return [found[i] for i in indices]


def sheet(args, columns, indices, dt, stem):
    """One render_compare still row per pose, stacked."""
    rows = [[(f"{label}, {index * dt / 1000:.1f} s", pictures[row].convert("RGB")) for label, pictures in columns]
            for row, index in enumerate(indices)]
    still_sheet(rows).save(args.out / f"{stem}-sheet.png", optimize=True)


def read_counts(path):
    with open(path, newline="") as handle:
        return [{key: int(value) for key, value in row.items()} for row in csv.DictReader(handle)]


def counts_chart(args, rows):
    fig, (clusters, triangles) = subplots(1, 2, FIGURE_SIZE)
    plot_counts(clusters, triangles, [row["ms"] / 1000 for row in rows], rows, args.label)
    for axis in (clusters, triangles):
        axis.set_xlabel("seconds along the camera path")
    save(fig, args.out / f"{args.stem}-counts.png")


def means_table(args, rows):
    def mean(key):
        return sum(row[key] for row in rows) / len(rows)

    submitted = mean("triangles_submitted")
    lines = [
        ("Nodes visited / cut", f"{mean('nodes_visited'):.0f} / {mean('nodes_cut'):.0f}"),
        ("Clusters tested / kept", f"{mean('clusters_tested'):.0f} / {mean('clusters_kept'):.0f}"),
        ("Kept clusters: nothing set up / hidden / drawn",
         f"{mean('clusters_not_set_up'):.0f} / {mean('clusters_hidden'):.0f} / {mean('clusters_drawn'):.0f}"),
        ("Triangles submitted", f"{submitted:.0f}"),
    ]
    for label, key in (("behind the near plane", "triangles_behind"), ("clipped", "triangles_clipped"),
                       ("miss every pixel centre", "triangles_missed"), ("facing away", "triangles_facing_away"),
                       ("set up", "triangles_set_up"), ("covering pixels", "triangles_covering")):
        lines.append((f"Submitted triangles {label}", f"{mean(key):.0f} ({PERCENT * mean(key) / submitted:.0f} %)"))
    body = markdown(["Per frame, path mean", args.label], lines)
    (args.tables / f"{args.stem}.md").write_text(body + "\n", encoding="utf-8")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--scene", required=True, type=pathlib.Path)
    parser.add_argument("--object", required=True)
    parser.add_argument("--camera", required=True)
    parser.add_argument("--stem", required=True)
    parser.add_argument("--label", required=True)
    parser.add_argument("--level", required=True, type=int, help="the tree depth whose boxes the sheet draws")
    parser.add_argument("--out", required=True, type=pathlib.Path)
    parser.add_argument("--tables", required=True, type=pathlib.Path)
    parser.add_argument("--work", required=True, type=pathlib.Path)
    parser.add_argument("--overdraw", metavar="STEM", help="also write STEM-sheet.png: the poses in landscape, "
                        "the camera's picture beside the overdraw view")
    args = parser.parse_args(argv)
    for folder in (args.out, args.tables, args.work):
        folder.mkdir(parents=True, exist_ok=True)

    dt = path_ms(args) // FRAMES
    counts = args.work / f"{args.stem}-counts.csv"
    viewer(args, *cull(), "--counts", str(counts), "--frames", str(FRAMES), "--dt", str(dt),
           "-o", str(args.work / "counts.bmp"), log=args.work / f"{args.stem}-counts.log")
    rows = read_counts(counts)

    shaded = video(args, "camera", ["--view", "shaded"], dt)
    top = video(args, "top", cull(("from", cull_from("TOP"))), dt)
    chase = video(args, "chase", cull(("from", cull_from("CHASE"))), dt)
    level = video(args, "level", cull(("from", cull_from("TOP")), ("paint", enum_value("raster_cull_paint_t", "RASTER_CULL_PAINT_", "NODES")), ("level", args.level)), dt)
    indices = [FRAMES * (k + 1) // (POSES + 1) for k in range(POSES)]
    columns = [("camera", frames_at(shaded, indices)), ("fates from above", frames_at(top, indices)),
               ("fates from behind", frames_at(chase, indices)),
               (f"level {args.level} from above", frames_at(level, indices))]
    sheet(args, columns, indices, dt, args.stem)
    if args.overdraw:
        turned = ("--quarter", str(read_define(DISPLAY_HEADER, "DISPLAY_LANDSCAPE")))
        sheet(args, [("camera", frames_at(video(args, "landscape", ["--view", "shaded"], dt, turned), indices)),
                     ("overdraw", frames_at(video(args, "overdraw", ["--view", "overdraw"], dt, turned), indices))],
              indices, dt, args.overdraw)

    counts_chart(args, rows)
    means_table(args, rows)


if __name__ == "__main__":
    main()
