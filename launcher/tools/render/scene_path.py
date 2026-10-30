#!/usr/bin/env python3
"""A scene's camera path rendered whole as an A | B video, or one frame of it, behind scene_viewer.sh
--compare and --frame.

    scene_path.py video --out OUT.mp4 --work DIR [--frames N] [--dt MS] --side LABEL HOST ASSETS ARGS --side ...
    scene_path.py frame N --out OUT.png --work DIR [--frames N] [--dt MS] --side LABEL HOST ASSETS ARGS [--side ...]

A side is a scene viewer HOST drawing the pack in ASSETS with its renderer arguments ARGS, one
per line. Both commands time the path alike, so frame N of a video is what `frame N` renders:
without --dt, the viewer's own frame time, or the shortest whole millisecond at which the path
fits one video when it does not at that; without --frames, from the start of the path to its end.

`video` renders every side's path, printing progress, and writes render_compare.py's A | B |
difference video, each frame labelled with its index and time. `frame` writes frame N of the
first side to OUT and of the second beside it, OUT's stem with -b. Each prints what it wrote.
"""

import argparse
import math
import os
import pathlib
import re
import subprocess
import sys
import time

from PIL import Image

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

from render_compare import GAIN, compare_videos  # noqa: E402

SIDES = ("a", "b")
# How often the render's progress line is redrawn, in seconds.
PROGRESS_SECONDS = 0.5
CAMERA_PATH = re.compile(r"camera path (\d+) ms")
FRAMES_FIT = re.compile(r"at most (\d+) frames fit")
# More frames than any video holds: a viewer asked for them only says how many fit, rendering none.
NEVER_FITS = 1 << 30


class Side:
    def __init__(self, label, host, assets, lines):
        self.label, self.host, self.assets = label.strip(), pathlib.Path(host), pathlib.Path(assets)
        self.args = [line for line in lines.split("\n") if line]


def host_run(side, args, log):
    env = dict(os.environ, AUTANA_ASSET_DIR=str(side.assets))
    return subprocess.Popen([str(side.host), *side.args, *args], stdout=subprocess.DEVNULL, stderr=log, env=env)


def host_text(side, args, log_path):
    with open(log_path, "w") as log:
        code = host_run(side, args, log).wait()
    return code, pathlib.Path(log_path).read_text()


def timing(side, work, frames=None, dt_ms=None):
    """(frames, dt_ms) of the path as both commands render it."""
    rows = work / "probe.csv"
    code, text = host_text(side, ["--frames", "2", "--poses", str(rows), "-o", str(work / "probe.bmp")],
                           work / "probe.log")
    found = CAMERA_PATH.search(text)
    if code or found is None:
        raise SystemExit(f"scene_path: the {side.label} viewer failed:\n{text}")
    duration_ms = int(found.group(1))
    if dt_ms is None:
        with open(rows) as handle:
            dt_ms = int(handle.readlines()[2].split(",")[1])  # frame 1's time
        _, text = host_text(side, ["--frames", str(NEVER_FITS), "--dt", str(dt_ms), "--video", str(work / "fit.avi")],
                            work / "fit.log")
        fits = int(FRAMES_FIT.search(text).group(1))
        if duration_ms // dt_ms + 1 > fits:
            dt_ms = math.ceil(duration_ms / (fits - 1))
            print(f"scene_path: the {duration_ms} ms path fits one video at --dt {dt_ms}", file=sys.stderr)
    return frames or duration_ms // dt_ms + 1, dt_ms


def lines_in(path):
    try:
        with open(path, "rb") as handle:
            return handle.read().count(b"\n")
    except OSError:
        return 0


def render(sides, frames, dt_ms, work):
    """Every side's path into work/<side>.avi at once, printing how far each has got."""
    names = SIDES[:len(sides)]
    for name in names:
        (work / f"{name}.csv").unlink(missing_ok=True)  # the progress count reads it
    logs = {name: open(work / f"{name}.log", "w") for name in names}
    runs = {name: host_run(side, ["--frames", str(frames), "--dt", str(dt_ms), "--video", str(work / f"{name}.avi"),
                                  "--poses", str(work / f"{name}.csv"), "-o", str(work / f"{name}.bmp")], logs[name])
            for name, side in zip(names, sides)}
    started = time.monotonic()
    while any(run.poll() is None for run in runs.values()):
        done = "  ".join(f"{name.upper()} {max(0, lines_in(work / f'{name}.csv') - 1)}/{frames}" for name in names)
        print(f"\rscene_path: rendering {done}  {time.monotonic() - started:.0f} s", end="", file=sys.stderr)
        time.sleep(PROGRESS_SECONDS)
    print(file=sys.stderr)
    for log in logs.values():
        log.close()
    for name, side in zip(names, sides):
        # A frame-watch verdict fails the run after every frame is written: the frames still stand.
        if lines_in(work / f"{name}.csv") - 1 != frames:
            raise SystemExit(f"scene_path: the {side.label} viewer failed:\n{(work / f'{name}.log').read_text()}")
        if runs[name].returncode:
            print(f"scene_path: the {side.label} viewer exited {runs[name].returncode}; see {work / f'{name}.log'}",
                  file=sys.stderr)
    print(f"scene_path: rendered {frames} frames per side at --dt {dt_ms} in {time.monotonic() - started:.0f} s",
          file=sys.stderr)


def side_path(out, index):
    """Where `frame` writes side `index`: OUT for the first, OUT's stem with -b for the second."""
    out = pathlib.Path(out)
    return out if index == 0 else out.with_name(f"{out.stem}-{SIDES[index]}{out.suffix}")


def write_frame(sides, index, frames, dt_ms, work, out):
    if not 0 <= index < frames:
        raise SystemExit(f"scene_path: the path has frames 0 to {frames - 1} at --dt {dt_ms}, not {index}")
    written = []
    for number, (name, side) in enumerate(zip(SIDES, sides)):
        bmp = work / f"frame-{name}.bmp"
        code, text = host_text(side, ["--frames", str(index + 1), "--dt", str(dt_ms), "-o", str(bmp)],
                               work / f"frame-{name}.log")
        if code and not bmp.is_file():
            raise SystemExit(f"scene_path: the {side.label} viewer failed:\n{text}")
        target = side_path(out, number)
        with Image.open(bmp) as picture:
            picture.save(target)
        written.append(target)
    return written


def parser():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    commands = parser.add_subparsers(dest="command", required=True)
    for name in ("video", "frame"):
        command = commands.add_parser(name)
        if name == "frame":
            command.add_argument("index", type=int)
        command.add_argument("--out", required=True)
        command.add_argument("--work", required=True)
        command.add_argument("--frames", type=int)
        command.add_argument("--dt", type=int)
        command.add_argument("--side", nargs=4, action="append", required=True,
                             metavar=("LABEL", "HOST", "ASSETS", "ARGS"))
    return parser


def main(argv=None):
    args = parser().parse_args(argv)
    sides = [Side(*side) for side in args.side]
    if len(sides) > len(SIDES) or (args.command == "video" and len(sides) != len(SIDES)):
        raise SystemExit(f"scene_path: {args.command} takes {'two' if args.command == 'video' else 'one or two'} --side")
    work = pathlib.Path(args.work)
    work.mkdir(parents=True, exist_ok=True)
    pathlib.Path(args.out).parent.mkdir(parents=True, exist_ok=True)
    frames, dt_ms = timing(sides[0], work, args.frames, args.dt)
    if args.command == "frame":
        for path in write_frame(sides, args.index, frames, dt_ms, work, args.out):
            print(path)
        return 0
    render(sides, frames, dt_ms, work)
    print(compare_videos(str(work / "a.avi"), str(work / "b.avi"), args.out, None, None, GAIN,
                         sides[0].label, sides[1].label), file=sys.stderr)
    print(args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
