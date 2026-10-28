"""How many hidden clusters a whole-cluster depth test before the draw could skip, at every pose a frame profile
replayed: the host renderer's cull view with cullview.occlusion (render/view/raster_cull_occlusion.h), kept as a
data file and tabled in the cull's section of Mesh-Rendering.md."""
import argparse
import concurrent.futures
import os
import pathlib
import re
import shlex
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[5]
TOOLS = ROOT / "launcher/tools"
sys.path[:0] = [str(TOOLS), str(TOOLS / "perf"), str(TOOLS / "render")]
from capture import QUARTERS
from generated_blocks import replace_block
import worst_frame_views as views

SCRIPT = pathlib.Path(__file__).resolve().relative_to(ROOT).as_posix()
HEADER = ROOT / "launcher/main/render/view/raster_cull_occlusion.h"
CAPTURE = ROOT / "docs/render/data/frame-profile-1-board.log"
DATA = ROOT / "docs/render/data/cluster-occlusion.tsv"
DOCUMENT = ROOT / "docs/render/Mesh-Rendering.md"
BLOCK = "cluster-occlusion"
VIEW = "cull"
OPTIONS = ("--set", "cullview.occlusion=1")
PERCENT = 100


def line_fields():
    """RASTER_OCCLUSION_LINE's field names, in its order."""
    text = HEADER.read_text(encoding="utf-8")
    line = re.search(r"#define RASTER_OCCLUSION_LINE((?:.*\\\n)*.*)", text)
    if line is None:
        raise ValueError(f"{HEADER}: no RASTER_OCCLUSION_LINE")
    return tuple(re.findall(r"(\w+)=%", line[1]))


FIELDS = line_fields()
BOUNDS = tuple(dict.fromkeys(name.split("_")[0] for name in FIELDS if name.endswith("_caught")
                             and not name.endswith("_hidden_caught")))
LINE_RE = re.compile(r"^occlusion: " + " ".join(rf"{name}=(\d+)" for name in FIELDS) + "$", re.M)
COLUMNS = views.COLUMNS + FIELDS


def measure(renderer, pose, out):
    stderr = views.draw(renderer, pose, VIEW, out, OPTIONS)
    lines = LINE_RE.findall(stderr)
    if len(lines) != 1:
        raise ValueError(f"{' '.join(map(str, pose))}: {len(lines)} occlusion lines")
    return dict(zip(FIELDS, map(int, lines[0])))


def render_all(text):
    """Draws every pose the capture replayed and writes the data file."""
    renderer = views.build()
    poses = sorted(views.wanted(text, scenes(text)))
    with tempfile.TemporaryDirectory() as tmp, concurrent.futures.ThreadPoolExecutor(os.cpu_count()) as pool:
        out = pathlib.Path(tmp)
        counts = list(pool.map(lambda p: measure(renderer, p, out / f"{'-'.join(map(str, p))}.bmp"), poses))
    lines = [views.provenance() + f" of the poses in {CAPTURE.name}", "\t".join(COLUMNS)]
    lines.extend("\t".join(map(str, pose + tuple(c[name] for name in FIELDS))) for pose, c in zip(poses, counts))
    DATA.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")


def read():
    """[(scene, quarter, pose, t_ms, {field: count})] from the data file."""
    rows = [line.split("\t") for line in DATA.read_text(encoding="utf-8").splitlines() if not line.startswith("#")]
    if not rows or tuple(rows[0]) != COLUMNS:
        raise ValueError(f"{DATA}: columns are not {', '.join(COLUMNS)}")
    return [(scene, quarter, pose, int(t_ms), dict(zip(FIELDS, map(int, counts))))
            for scene, quarter, pose, t_ms, *counts in rows[1:]]


def scenes(text):
    """The capture's scenes, in its order."""
    return list(dict.fromkeys(row for row, _ in views.path_poses(text)))


def share(part, whole):
    return "–" if whole == 0 else f"{PERCENT * part / whole:.0f} %"


def table(text):
    """The block's body; fails when the data file's poses are not the capture's or a bound skipped a cluster that
    drew."""
    rows = read()
    measured = {row[:4] for row in rows}
    wanted = views.wanted(text, scenes(text))
    if measured != wanted:
        raise ValueError(f"{DATA.name}: {len(wanted - measured)} poses missing and {len(measured - wanted)} not in "
                         f"the capture; run with --render")
    unsafe = [row[:4] for row in rows if any(row[4][f"{bound}_unsafe"] for bound in BOUNDS)]
    if unsafe:
        raise ValueError(f"{DATA.name}: a bound skipped a cluster that wrote depth at {unsafe[0]} and "
                         f"{len(unsafe) - 1} more poses")
    lines = ["| Scene | Quarter | Poses | Hidden clusters / frame | "
             + " | ".join(f"{bound}: hidden caught | {bound}: set up removed | {bound}: read / frame"
                          for bound in BOUNDS) + " |",
             "|---|---|---:|---:|" + "---:|---:|---:|" * len(BOUNDS)]
    for scene in scenes(text):
        for quarter in QUARTERS:
            group = [row[4] for row in rows if row[0] == scene and row[1] == quarter]
            total = {name: sum(counts[name] for counts in group) for name in FIELDS}
            cells = [scene, quarter, str(len(group)), f"{total['hidden'] / len(group):.0f}"]
            for bound in BOUNDS:
                cells += [share(total[f"{bound}_hidden_caught"], total["hidden"]),
                          share(total[f"{bound}_removed"], total["triangles_set_up"]),
                          f"{total[f'{bound}_read'] / len(group):.0f}"]
            lines.append("| " + " | ".join(cells) + " |")
    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--render", action="store_true", help="draw every pose on the host again, rewriting "
                        f"{DATA.name}")
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    command = shlex.join(["python", SCRIPT, "--check"])
    try:
        text = CAPTURE.read_text(encoding="utf-8")
        if args.render and not args.check:
            render_all(text)
        changed = replace_block(DOCUMENT, BLOCK, table(text), check=args.check, command=command)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    if args.check and changed:
        print(f"{DOCUMENT}: {BLOCK} differs; run the generator")
    return int(args.check and changed)


if __name__ == "__main__":
    sys.exit(main())
