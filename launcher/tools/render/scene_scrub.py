#!/usr/bin/env python3
"""The scrub window behind scene_viewer.sh --live, and the replay of its marks behind --mark.

    scene_scrub.py live --scene-file SCENE.scene.toml --repo ROOT --out DIR --marks DIR
        [--frames N] [--dt MS] [--shot FRAME PNG]
        --side LABEL TREE HOST ASSETS ARGS [--side LABEL TREE HOST ASSETS ARGS]
    scene_scrub.py replay MARK.json --repo ROOT [--side a|b]

`live` renders each side's whole camera path once, both at the same frame times, into
DIR/<side>.avi with the camera pose of every frame in DIR/<side>.csv, printing progress, then
opens a window over the frames. A side is a scene viewer HOST drawing the pack in ASSETS, which
was built from the checkout TREE, with its renderer arguments ARGS, one per line. Without --frames and --dt the path is rendered at the viewer's own frame time, from
its start to its end; a path too long for one video at that time is rendered at the shortest
whole-millisecond time that fits, which is said.

Left and Right step a frame, ten with Shift; Home and End go to the ends, as the slider
does. Space shows the other side at the same frame. M writes a mark into the --marks folder,
prints its path and shows it in the status line. Escape closes the window. --shot writes the
window's picture of each side at FRAME, side by side, to PNG instead of opening it.

A mark, <scene id>-f<frame>.json, holds the scene file (relative to the checkout when inside it), the frame,
its time and frame time, and per side its label, revision, renderer arguments, the sha256 of
each bake its checkout locks for the scene (launcher/bakes.lock), the camera pose and a PNG of
the frame beside the mark. `replay` prints the scene file as the mark holds it and the arguments that render that
side's frame again in this checkout, one per line, and warns on stderr when the checkout's
bakes for the scene differ from the side's.
"""

import argparse
import dataclasses
import json
import math
import mmap
import os
import pathlib
import re
import subprocess
import sys
import time
import tomllib

from PIL import Image, ImageDraw

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

from check_avi import parse_avi  # noqa: E402
from render_compare import _font  # noqa: E402

SIDES = ("a", "b")
# How often the render's progress line is redrawn, in seconds.
PROGRESS_SECONDS = 0.5
# Frames a Shift+arrow steps.
FAST_STEP = 10
# Window pixels per rendered pixel, and the status strip under the picture, in window pixels.
ZOOM = 2
STATUS_HEIGHT = 24
STATUS_PADDING = 4
STATUS_INK = (230, 230, 230)
STATUS_PAPER = (24, 24, 24)
# --shot's gap between the sides' pictures, in pixels, and its colour.
SHOT_GAP = 8
SHOT_PAPER = (255, 255, 255)
POSE_FIELDS = ("px", "py", "pz", "qx", "qy", "qz", "qw")
BAKES_LOCK = pathlib.Path("launcher/bakes.lock")
CAMERA_PATH = re.compile(r"camera path (\d+) ms")
FRAMES_FIT = re.compile(r"at most (\d+) frames fit")


@dataclasses.dataclass(frozen=True)
class Pose:
    position: tuple
    rotation: tuple  # quaternion x, y, z, w


@dataclasses.dataclass
class Side:
    """One built viewer, its arguments and, once rendered, its frames."""
    label: str
    scene_id: str
    tree: pathlib.Path
    host: pathlib.Path
    assets: pathlib.Path
    args: list
    revision: str = ""
    bakes: dict = dataclasses.field(default_factory=dict)
    frames: list = dataclasses.field(default_factory=list)
    poses: list = dataclasses.field(default_factory=list)
    width: int = 0
    height: int = 0

    def picture(self, frame):
        """The frame as an RGB image (AVI frames are bottom-up BGR rows padded to 4 bytes)."""
        stride = (self.width * 3 + 3) // 4 * 4
        return Image.frombuffer("RGB", (self.width, self.height), bytes(self.frames[frame]), "raw", "BGR",
                                stride, -1)


class Scrub:
    """Where the window is: a frame of the path and the side shown. No window toolkit here."""

    def __init__(self, scene_file, scene_id, dt_ms, sides):
        self.scene_file, self.scene_id, self.dt_ms, self.sides = scene_file, scene_id, dt_ms, sides
        self.count = min(len(side.frames) for side in sides)
        self.frame = 0
        self.showing = 0
        self.note = ""

    def seek(self, frame):
        self.frame = max(0, min(self.count - 1, frame))
        self.note = ""

    def step(self, by):
        self.seek(self.frame + by)

    def swap(self):
        self.showing = (self.showing + 1) % len(self.sides)
        self.note = ""

    def shown(self):
        return self.sides[self.showing]

    def status(self):
        side = self.shown()
        text = (f"frame {self.frame}/{self.count - 1}  {self.frame * self.dt_ms / 1000:.3f} s  "
                f"{SIDES[self.showing].upper()}: {side.label}")
        return f"{text}  {self.note}" if self.note else text

    def mark(self, folder):
        """Writes the mark of the current frame and a PNG per side; returns the mark's path."""
        folder = pathlib.Path(folder)
        folder.mkdir(parents=True, exist_ok=True)
        stem = f"{self.scene_id}-f{self.frame:05d}"
        record = {"scene": self.scene_file, "scene_id": self.scene_id, "frame": self.frame,
                  "ms": self.frame * self.dt_ms, "dt_ms": self.dt_ms, "sides": {}}
        for name, side in zip(SIDES, self.sides):
            png = f"{stem}-{name}.png"
            side.picture(self.frame).save(folder / png)
            pose = side.poses[self.frame]
            record["sides"][name] = {"label": side.label, "revision": side.revision, "args": side.args,
                                     "bakes": side.bakes, "pose": dataclasses.asdict(pose), "png": png}
        path = folder / f"{stem}.json"
        path.write_text(json.dumps(record, indent=2) + "\n")
        self.note = f"marked {path}"
        return path


def compose(scrub):
    """The window's picture: the shown frame enlarged, over a strip saying where it is."""
    frame = scrub.shown().picture(scrub.frame)
    frame = frame.resize((frame.width * ZOOM, frame.height * ZOOM), Image.NEAREST)
    picture = Image.new("RGB", (frame.width, frame.height + STATUS_HEIGHT), STATUS_PAPER)
    picture.paste(frame, (0, 0))
    ImageDraw.Draw(picture).text((STATUS_PADDING, frame.height + STATUS_PADDING), scrub.status(),
                                 fill=STATUS_INK, font=_font())
    return picture


def shot(scrub):
    """The window's picture of every side at the current frame, left to right."""
    pictures = []
    for _ in scrub.sides:
        pictures.append(compose(scrub))
        scrub.swap()
    sheet = Image.new("RGB", (sum(p.width for p in pictures) + SHOT_GAP * (len(pictures) - 1),
                              max(p.height for p in pictures)), SHOT_PAPER)
    x = 0
    for picture in pictures:
        sheet.paste(picture, (x, 0))
        x += picture.width + SHOT_GAP
    return sheet


def scene_bakes(tree, scene_file):
    """Each locked bake of the scene in `tree`, output name to sha256; empty without a lock."""
    lock = pathlib.Path(tree) / BAKES_LOCK
    if not lock.is_file():
        return {}
    rows = tomllib.loads(lock.read_text()).get("bake", [])
    return {row["output"]: row["sha256"] for row in rows if row.get("source") == scene_file}


def revision(tree):
    """The commit `tree` has checked out, +dirty with uncommitted changes; the path outside git."""
    head = subprocess.run(["git", "-C", str(tree), "rev-parse", "HEAD"], capture_output=True, text=True)
    if head.returncode:
        return str(tree)
    dirty = subprocess.run(["git", "-C", str(tree), "status", "--porcelain", "--untracked-files=no"],
                           capture_output=True, text=True).stdout.strip()
    return head.stdout.strip() + ("+dirty" if dirty else "")


def read_poses(path):
    poses = []
    with open(path) as rows:
        header = rows.readline().strip().split(",")
        at = [header.index(field) for field in POSE_FIELDS]
        for line in rows:
            values = [float(value) for value in line.strip().split(",")]
            poses.append(Pose(tuple(values[i] for i in at[:3]), tuple(values[i] for i in at[3:])))
    return poses


def host_run(side, args, log):
    env = dict(os.environ, AUTANA_ASSET_DIR=str(side.assets))
    return subprocess.Popen([str(side.host), "--scene", side.scene_id, *side.args, *args], stdout=subprocess.DEVNULL,
                            stderr=log, env=env)


def probe(side, out):
    """The side's camera path length and its viewer's own frame time, from a two-frame render."""
    rows, log = out / "probe.csv", out / "probe.log"
    with open(log, "w") as handle:
        code = host_run(side, ["--frames", "2", "--poses", str(rows), "-o", str(out / "probe.bmp")], handle).wait()
    text = log.read_text()
    found = CAMERA_PATH.search(text)
    if code or found is None:
        raise SystemExit(f"scene_scrub: the {side.label} viewer failed:\n{text}")
    with open(rows) as handle:
        dt_ms = int(handle.readlines()[2].split(",")[1])
    return int(found.group(1)), dt_ms


class TooLong(Exception):
    def __init__(self, fits):
        super().__init__(fits)
        self.fits = fits


def lines_in(path):
    try:
        with open(path, "rb") as handle:
            return handle.read().count(b"\n")
    except OSError:
        return 0


def render(sides, frames, dt_ms, out):
    """Renders every side's frames at once, printing how far each has got."""
    names = SIDES[:len(sides)]
    for name in names:
        (out / f"{name}.csv").unlink(missing_ok=True)  # the progress count reads it
    logs = {name: open(out / f"{name}.log", "w") for name in names}
    runs = {name: host_run(side, ["--frames", str(frames), "--dt", str(dt_ms), "--video", str(out / f"{name}.avi"),
                                  "--poses", str(out / f"{name}.csv"), "-o", str(out / f"{name}.bmp")], logs[name])
            for name, side in zip(names, sides)}
    started = time.monotonic()
    while any(run.poll() is None for run in runs.values()):
        done = "  ".join(f"{name.upper()} {max(0, lines_in(out / f'{name}.csv') - 1)}/{frames}" for name in names)
        print(f"\rscene_scrub: rendering {done}  {time.monotonic() - started:.0f} s", end="", file=sys.stderr)
        time.sleep(PROGRESS_SECONDS)
    print(file=sys.stderr)
    for log in logs.values():
        log.close()
    for name, side in zip(names, sides):
        text = (out / f"{name}.log").read_text()
        fits = FRAMES_FIT.search(text)
        if fits:
            raise TooLong(int(fits.group(1)))
        # A frame-watch verdict fails the run after every frame is written: the frames still stand.
        if lines_in(out / f"{name}.csv") - 1 != frames:
            raise SystemExit(f"scene_scrub: the {side.label} viewer failed:\n{text}")
        if runs[name].returncode:
            print(f"scene_scrub: the {side.label} viewer exited {runs[name].returncode}; see {out / f'{name}.log'}",
                  file=sys.stderr)
    print(f"scene_scrub: rendered {frames} frames per side at --dt {dt_ms} in {time.monotonic() - started:.0f} s",
          file=sys.stderr)


def load(side, avi, poses):
    with open(avi, "rb") as handle:
        data = mmap.mmap(handle.fileno(), 0, access=mmap.ACCESS_READ)
    movie = parse_avi(data)
    side.width, side.height, side.frames = movie.width, movie.height, list(movie.frames)
    side.poses = read_poses(poses)


def live(scrub, marks):
    import tkinter
    from PIL import ImageTk

    window = tkinter.Tk()
    window.title(f"scene_viewer: {scrub.scene_id}")
    picture = tkinter.Label(window)
    picture.pack()
    slider = tkinter.Scale(window, from_=0, to=scrub.count - 1, orient="horizontal", showvalue=False)
    slider.pack(fill="x")

    def show():
        image = ImageTk.PhotoImage(compose(scrub))
        picture.configure(image=image)
        picture.image = image
        slider.set(scrub.frame)

    def act(change):
        def handler(_event):
            change()
            show()
        return handler

    def mark():
        print(scrub.mark(marks), flush=True)

    slider.configure(command=lambda value: (scrub.seek(int(value)), show()) if int(value) != scrub.frame else None)
    for key, change in (("<Left>", lambda: scrub.step(-1)), ("<Right>", lambda: scrub.step(1)),
                        ("<Shift-Left>", lambda: scrub.step(-FAST_STEP)),
                        ("<Shift-Right>", lambda: scrub.step(FAST_STEP)),
                        ("<Home>", lambda: scrub.seek(0)), ("<End>", lambda: scrub.seek(scrub.count - 1)),
                        ("<space>", scrub.swap), ("<m>", mark), ("<M>", mark)):
        window.bind(key, act(change))
    window.bind("<Escape>", lambda _event: window.destroy())
    show()
    window.mainloop()


def checkout_path(path, repo):
    """`path` relative to the checkout `repo`, or whole when it lies outside it, with forward slashes."""
    path = pathlib.Path(path).resolve()
    return (path.relative_to(repo) if path.is_relative_to(repo) else path).as_posix()


def rendered(args):
    """The Scrub over every --side's frames, rendered into --out."""
    out = pathlib.Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    repo = pathlib.Path(args.repo).resolve()
    scene_file = checkout_path(args.scene_file, repo)
    scene_id = pathlib.Path(scene_file).name.removesuffix(".scene.toml")
    sides = []
    for label, tree, host, assets, lines in args.side:
        side = Side(label, scene_id, pathlib.Path(tree), pathlib.Path(host), pathlib.Path(assets),
                    [line for line in lines.split("\n") if line])
        side.revision, side.bakes = revision(side.tree), scene_bakes(side.tree, scene_file)
        sides.append(side)
    duration_ms, dt_ms = probe(sides[0], out)
    dt_ms = args.dt or dt_ms
    frames = args.frames or duration_ms // dt_ms + 1
    try:
        render(sides, frames, dt_ms, out)
    except TooLong as refusal:
        if args.frames:
            raise SystemExit(f"scene_scrub: --frames {frames} does not fit one video; at most {refusal.fits} do")
        dt_ms = math.ceil(duration_ms / (refusal.fits - 1))
        frames = duration_ms // dt_ms + 1
        print(f"scene_scrub: the {duration_ms} ms path fits one video at --dt {dt_ms}", file=sys.stderr)
        render(sides, frames, dt_ms, out)
    for name, side in zip(SIDES, sides):
        load(side, out / f"{name}.avi", out / f"{name}.csv")
    return Scrub(scene_file, scene_id, dt_ms, sides)


def run_live(args):
    scrub = rendered(args)
    if args.shot:
        scrub.seek(int(args.shot[0]))
        shot(scrub).save(args.shot[1])
        return 0
    print(f"scene_scrub: marks go to {pathlib.Path(args.marks).resolve()}", file=sys.stderr)
    live(scrub, args.marks)
    return 0


def replay_lines(mark_path, side, repo):
    """The scene file, as the mark holds it, and the viewer arguments that render the marked frame of `side`
    again."""
    record = json.loads(pathlib.Path(mark_path).read_text())
    marked = record["sides"].get(side)
    if marked is None:
        raise SystemExit(f"scene_scrub: {mark_path} has no side {side}")
    here = scene_bakes(repo, record["scene"])
    if here != marked["bakes"]:
        changed = sorted(set(here.items()) ^ set(marked["bakes"].items()))
        print(f"scene_scrub: this checkout's bakes differ from side {side} ({marked['revision']}): "
              + ", ".join(sorted({name for name, _ in changed})), file=sys.stderr)
    return [record["scene"], *marked["args"],
            "--frames", str(record["frame"] + 1), "--dt", str(record["dt_ms"])]


def parser():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    commands = parser.add_subparsers(dest="command", required=True)
    live_parser = commands.add_parser("live")
    live_parser.add_argument("--scene-file", required=True)
    live_parser.add_argument("--repo", required=True)
    live_parser.add_argument("--out", required=True)
    live_parser.add_argument("--marks", required=True)
    live_parser.add_argument("--frames", type=int)
    live_parser.add_argument("--dt", type=int)
    live_parser.add_argument("--shot", nargs=2, metavar=("FRAME", "PNG"))
    live_parser.add_argument("--side", nargs=5, action="append", required=True,
                             metavar=("LABEL", "TREE", "HOST", "ASSETS", "ARGS"))
    replay_parser = commands.add_parser("replay")
    replay_parser.add_argument("mark")
    replay_parser.add_argument("--repo", required=True)
    replay_parser.add_argument("--side", choices=SIDES, default=SIDES[0])
    return parser


def main(argv=None):
    args = parser().parse_args(argv)
    if args.command == "replay":
        sys.stdout.reconfigure(newline="\n")  # read by sh, which would keep a carriage return
        print("\n".join(replay_lines(args.mark, args.side, pathlib.Path(args.repo))))
        return 0
    if len(args.side) > len(SIDES):
        raise SystemExit(f"scene_scrub: at most {len(SIDES)} --side")
    return run_live(args)


if __name__ == "__main__":
    sys.exit(main())
