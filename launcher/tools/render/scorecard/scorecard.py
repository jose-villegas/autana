#!/usr/bin/env python3
"""Measure and compare the fitted rendering track across two engine revisions."""
import argparse
import json
import pathlib
import subprocess
import sys
import tempfile

from captures import labeled_capture, scene_captures, suite_rows
from report import overlay_callable, write_report

SCRIPT = pathlib.Path(__file__).resolve()
REPO = SCRIPT.parents[4]
TOOLS = SCRIPT.parents[2]
RENDER = TOOLS / "render"
RESULTS = TOOLS / "results" / "scorecard"
ENTRY = SCRIPT.relative_to(REPO)
SCENES = ("sponza", "bistro")
POSE_COUNT = 4
POSE_SAMPLE_MS = 100
LANDSCAPE_QUARTER = "1"


def git(tree, *args):
    return subprocess.check_output(["git", "-C", tree, *args], text=True).strip()


def parse_capture(spec):
    label, equals, path = spec.partition("=")
    if not equals or not label or not path:
        raise ValueError(f"capture {spec!r} needs LABEL=PATH")
    return label, pathlib.Path(path).resolve()


def render_poses(scene, output):
    import numpy as np
    from PIL import Image

    from check_avi import read_video
    from pinhole_rays import VIDEO_SCALE
    from r3d.poses import flight_camera, path_poses
    from r3d.reference_set import poses_text
    from see_through import RENDER_SIZE

    width, height, lens, near, path = path_poses(flight_camera(scene), RENDER_SIZE, POSE_SAMPLE_MS, either_way_up=False)
    indices = np.linspace(0, len(path) - 1, POSE_COUNT).round().astype(int)
    poses = [path[index] for index in indices]
    pose_file = output / "poses.txt"
    pose_file.write_text(poses_text(width, height, lens, near, poses), encoding="utf-8")
    video = output / "poses.avi"
    subprocess.run(["sh", RENDER / "scene_viewer.sh", scene.path, "--camera-poses", pose_file,
                    "--size", f"{height}x{width}", "--quarter", LANDSCAPE_QUARTER, "-o", output / "last.bmp",
                    "--video", video], check=True, cwd=REPO)
    images = []
    for index, frame in enumerate(read_video(video)[1]):
        pixels = frame[::VIDEO_SCALE, ::VIDEO_SCALE]
        if pixels.shape[:2] != (height, width):
            raise ValueError(f"{scene.path}: unexpected output dimensions {pixels.shape[:2]}")
        picture = output / f"pose-{index}.png"
        Image.fromarray(pixels).save(picture)
        images.append(picture)
    video.unlink()
    if len(images) != len(poses):
        raise ValueError(f"{scene.path}: wanted {len(poses)} rendered poses, read {len(images)}")
    return images, [pose.tolist() for pose in poses], [width, height]


def measure_scene(key, capture, output):
    sys.path.insert(0, str(TOOLS.parent / "main/apps/render_lab/tools"))
    from bake import bake
    from r3d.appearance_simplify import start_mesh
    from r3d.build_pack import pack_bytes
    from r3d.fit.copies import joined, placed_copies
    from r3d.import_settings import load_scene
    from r3d.mesh_source import load_source
    from r3d.objects.held_out_reference import measure_held_out
    from r3d.objects.part_shape import measure_parts
    from see_through import measure_see_through, see_through_ceiling
    from sky_through_walls import MOST_HOLED_FRAMES, SkyUnavailable, measure_sky

    scene = load_scene(TOOLS.parent / "demo" / key / f"{key}.scene.toml")
    job, = (job for job in scene.renderers if job.renderer.fit)
    folder = output / key
    folder.mkdir(parents=True, exist_ok=True)
    ceilings = suite_rows(key)
    captures = scene_captures(capture, ceilings, key, job.renderer.variant.name)
    print(f"{key}: pack and see-through", flush=True)
    packs = pack_bytes([scene.path])
    pack_size = len(packs[key])
    del packs
    see_through = dict(pixels=sum(measure_see_through(scene)), ceiling=see_through_ceiling(key))
    print(f"{key}: sky holes", flush=True)
    try:
        with tempfile.TemporaryDirectory(dir=folder) as sky_work:
            holed, frames = measure_sky(scene, key, pathlib.Path(sky_work))
        sky = dict(holed=len(holed), frames=frames, ceiling=MOST_HOLED_FRAMES.get(key))
    except SkyUnavailable as error:
        sky = dict(reason=str(error))
    print(f"{key}: held-out probe score", flush=True)
    fit = bake.locked_fit(scene.path, job.object.name)
    cache = bake.default_cache()
    bundle = bake.fetch(fit, cache)
    rest = start_mesh(bundle)
    drawn = joined(rest, placed_copies(bundle, rest.flat))
    with tempfile.TemporaryDirectory(dir=folder) as work:
        held_out = measure_held_out(fit, cache, pathlib.Path(work), drawn)
    print(f"{key}: part shapes and comparison poses", flush=True)
    parts = measure_parts(load_source(job.settings, texture_dtype="float32"),
                          drawn.points[drawn.vertex_point], drawn.tris)
    images, poses, size = render_poses(scene, folder)
    return dict(path=scene.path.relative_to(REPO).as_posix(), ceilings=ceilings, captures=captures,
                pack_bytes=pack_size, see_through=see_through, sky=sky, held_out=held_out, parts=parts,
                images=[p.relative_to(output.parent).as_posix() for p in images], poses=poses, size=size)


def measure(out, specs, scenes):
    out = out.resolve()
    out.parent.mkdir(parents=True, exist_ok=True)
    readings = []
    for spec in specs:
        label, path = parse_capture(spec)
        readings.extend(labeled_capture(label, path))
    output = out.parent / f"{out.stem}-renders"
    output.mkdir(parents=True, exist_ok=True)
    result = dict(revision=git(REPO, "rev-parse", "HEAD"), scenes={})
    for key in scenes:
        result["scenes"][key] = measure_scene(key, readings, output)
    out.write_text(json.dumps(result, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    print(out)


def table(parent, tip, out, overlay=None, tip_tree=None):
    paths = (parent.resolve(), tip.resolve())
    revisions = [json.loads(path.read_text(encoding="utf-8")) for path in paths]
    if overlay and git(tip_tree, "rev-parse", "HEAD") != revisions[1]["revision"]:
        raise ValueError("--tip-tree does not match the measured tip revision")
    print(write_report(out, revisions, [path.parent for path in paths], overlay, tip_tree))


def compare(args):
    refs = {"parent": args.parent, "tip": args.tip}
    revisions = {arm: git(REPO, "rev-parse", "--verify", "--end-of-options", f"{ref}^{{commit}}") for arm, ref in refs.items()}
    for arm, revision in revisions.items():
        exists = subprocess.run(["git", "-C", REPO, "cat-file", "-e", f"{revision}:{ENTRY.as_posix()}"],
                                stderr=subprocess.DEVNULL)
        if exists.returncode:
            raise ValueError(f"{arm} {refs[arm]} ({revision}) has no scorecard at {ENTRY}")
    captures = []
    for arm, spec in args.capture:
        if arm not in refs:
            raise ValueError(f"capture arm {arm!r} must be parent or tip")
        label, path = parse_capture(spec)
        captures.extend(("--capture", arm, f"{label}={path}"))
    RESULTS.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=RESULTS) as scratch:
        # The shared checkout helper owns submodule setup and worktree removal.
        helper = TOOLS / "revision_worktree.sh"
        subprocess.run(
            ["sh", "-c", '''
                set -eu
                . "$1"
                revision_worktree_setup "$2" "$3"
                trap revision_worktree_cleanup EXIT
                parent=$(revision_worktree_checkout "$4" parent)
                tip=$(revision_worktree_checkout "$5" tip)
                shift 5
                "$@" "$parent" "$tip"
             ''', "scorecard", helper, REPO, scratch, *revisions.values(),
             sys.executable, SCRIPT, "_pair", "--out", args.out.resolve(),
             *captures, *(word for scene in args.scene or () for word in ("--scene", scene)),
             *(("--overlay", args.overlay) if args.overlay else ())], check=True)


def measure_pair(args):
    files = []
    scenes = args.scene or SCENES
    selection = [word for scene in scenes for word in ("--scene", scene)]
    for arm, tree in zip(("parent", "tip"), (args.parent_tree, args.tip_tree)):
        subprocess.run([sys.executable, tree / "launcher/tools/bake/bake.py", "fetch",
                        *(f"launcher/demo/{scene}" for scene in scenes)], cwd=tree, check=True)
        out = args.out / f"{arm}.json"
        captures = [word for side, spec in args.capture if side == arm for word in ("--capture", spec)]
        subprocess.run([sys.executable, tree / ENTRY, "measure", "--out", out, *captures, *selection],
                       cwd=tree, check=True)
        files.append(out)
    table(*files, args.out, overlay_callable(args.overlay) if args.overlay else None, args.tip_tree)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    measuring = commands.add_parser("measure", help="measure the current worktree")
    measuring.add_argument("--out", type=pathlib.Path, required=True)
    measuring.add_argument("--capture", action="append", default=[], metavar="LABEL=PATH")
    tabling = commands.add_parser("table", help="join existing revision JSONs and pictures")
    comparing = commands.add_parser("compare", help="measure two detached revisions and join them")
    pair = commands.add_parser("_pair")
    for command in (tabling, comparing, pair):
        command.add_argument("--out", type=pathlib.Path, required=True, help="report directory")
        command.add_argument("--overlay", metavar="MODULE:FUNCTION")
    for name in ("parent", "tip"):
        tabling.add_argument(f"--{name}", type=pathlib.Path, required=True)
        comparing.add_argument(f"--{name}", required=True)
        pair.add_argument(f"{name}_tree", type=pathlib.Path)
    tabling.add_argument("--tip-tree", type=pathlib.Path, default=REPO)
    for command in (measuring, comparing, pair):
        command.add_argument("--scene", action="append", choices=SCENES, help="select a scene (default: both)")
    for command in (comparing, pair):
        command.add_argument("--capture", nargs=2, action="append", default=[], metavar=("ARM", "LABEL=PATH"))
    args = parser.parse_args()
    try:
        if args.command == "measure":
            measure(args.out, args.capture, args.scene or SCENES)
        elif args.command == "table":
            table(args.parent, args.tip, args.out, overlay_callable(args.overlay) if args.overlay else None,
                  args.tip_tree.resolve())
        elif args.command == "compare":
            compare(args)
        else:
            measure_pair(args)
    except (OSError, RuntimeError, ValueError, ImportError, AttributeError, subprocess.SubprocessError) as error:
        parser.exit(1, f"scorecard: {error}\n")


if __name__ == "__main__":
    main()
