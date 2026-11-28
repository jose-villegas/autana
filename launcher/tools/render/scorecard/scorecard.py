#!/usr/bin/env python3
"""Measure and compare the fitted rendering track across two engine revisions."""
import argparse
from concurrent.futures import ProcessPoolExecutor, ThreadPoolExecutor
from contextlib import contextmanager
import json
import multiprocessing
import os
import pathlib
import subprocess
import sys
import tempfile
import time

from captures import labeled_capture, scene_captures
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
LAB_TOOLS = TOOLS.parent / "main/apps/render_lab/tools"
SOURCE_SAMPLES_ENV = "AUTANA_SCORECARD_SOURCE_SAMPLES"


def git(tree, *args):
    return subprocess.check_output(["git", "-C", tree, *args], text=True).strip()


def parse_capture(spec):
    label, equals, path = spec.partition("=")
    if not equals or not label or not path:
        raise ValueError(f"capture {spec!r} needs LABEL=PATH")
    return label, pathlib.Path(path).resolve()


def render_poses(scene, key, output, binary, assets):
    import numpy as np
    from PIL import Image

    from check_avi import read_video
    from pinhole_rays import VIDEO_SCALE
    from r3d.poses import flight_camera, path_poses
    from r3d.reference_set import poses_text
    from see_through import RENDER_SIZE

    width, height, lens, near, path = path_poses(flight_camera(scene), RENDER_SIZE, POSE_SAMPLE_MS, either_way_up=False)
    # A loop's last pose can repeat its opening view.
    indices = np.linspace(0, len(path), min(POSE_COUNT, len(path)), endpoint=False, dtype=int)
    poses = [path[index] for index in indices]
    pose_file = output / "poses.txt"
    pose_file.write_text(poses_text(width, height, lens, near, poses), encoding="utf-8")
    video = output / "poses.avi"
    subprocess.run([binary, "--scene", key, "--camera-poses", pose_file, "--size", f"{height}x{width}",
                    "--quarter", LANDSCAPE_QUARTER, "-o", output / "last.bmp", "--video", video],
                   check=True, cwd=REPO, env=dict(os.environ, AUTANA_ASSET_DIR=str(assets)))
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


@contextmanager
def timed_row(scene, row):
    """Log the wall time of a scorecard measurement in seconds."""
    start = time.perf_counter()
    try:
        yield
    finally:
        print(f"scorecard timing: {scene} | {row} | {time.perf_counter() - start:.3f} s", flush=True)


def scene_path(key):
    return TOOLS.parent / "demo" / key / f"{key}.scene.toml"


def measure_scene(key, capture, output, viewer, sky_binary, assets):
    sys.path.insert(0, str(LAB_TOOLS))
    from bake import bake
    from r3d.build_pack import pack_bytes, write_packs
    from r3d.fit.copies import drawn_mesh
    from r3d.import_settings import content_checksum, load_scene
    from held_out_reference import measure_held_out
    from r3d.objects.part_shape import measure_parts
    from see_through import SceneSources, measure_see_through, see_through_ceiling
    from source_sample_cache import source_sample_cache
    from sky_through_walls import MOST_HOLED_FRAMES, SkyUnavailable, measure_sky

    scene = load_scene(scene_path(key))
    sources = SceneSources(scene)
    job, = (job for job in scene.renderers if job.renderer.fit)
    folder = output / key
    folder.mkdir(parents=True, exist_ok=True)
    captures = scene_captures(capture, key)
    with timed_row(key, "pack bytes"):
        packs = pack_bytes([scene.path])
        pack_size = len(packs[key])
        write_packs(assets, packs)
        del packs
    with timed_row(key, "see-through"):
        see_through = dict(pixels=sum(measure_see_through(scene, sources)), ceiling=see_through_ceiling(key))
    with timed_row(key, "sky holes"):
        try:
            with tempfile.TemporaryDirectory(dir=folder) as sky_work:
                holed, frames = measure_sky(scene, key, pathlib.Path(sky_work), sky_binary, sources)
            sky = dict(holed=len(holed), frames=frames, ceiling=MOST_HOLED_FRAMES.get(key))
        except SkyUnavailable as error:
            sky = dict(reason=str(error))
    with timed_row(key, "held-out mean dE76"):
        fit = bake.locked_fit(scene.path, job.object.name)
        cache = bake.default_cache()
        with tempfile.TemporaryDirectory(dir=folder) as work:
            held_out = measure_held_out(fit, cache, pathlib.Path(work), drawn_mesh(bake.fetch(fit, cache)))
    with timed_row(key, "part shapes"):
        drawn = drawn_mesh(bake.fetch(bake.locked_bake(scene.path, job.object.name, "levels"), cache))
        cache_dir = os.environ.get(SOURCE_SAMPLES_ENV)
        source_oid = content_checksum(job.settings.source["path"]).decode() if cache_dir else None
        source = sources.mesh(job)
        del sources
        source.textures = ()
        with source_sample_cache(pathlib.Path(cache_dir) if cache_dir else None, source_oid) as sample:
            parts = measure_parts(source, drawn.points[drawn.vertex_point], drawn.tris, sample=sample)
    with timed_row(key, "comparison poses"):
        images, poses, size = render_poses(scene, key, folder, viewer, assets)
    return dict(path=scene.path.relative_to(REPO).as_posix(), captures=captures,
                pack_bytes=pack_size, see_through=see_through, sky=sky, held_out=held_out, parts=parts,
                images=[p.relative_to(output.parent).as_posix() for p in images], poses=poses, size=size)


def build_hosts(out, first_scene):
    """Build each host renderer once and share its executable across the scenes."""
    sys.path.insert(0, str(LAB_TOOLS))
    from render_host_build import run

    with timed_row("all scenes", "host viewers"):
        built = subprocess.run(["sh", RENDER / "scenes/scene_viewer_render_host.sh", "--build-only",
                                "--asset-file", scene_path(first_scene), "-o", out / "poses"],
                               capture_output=True, text=True)
        if built.returncode:
            raise RuntimeError((built.stdout + built.stderr)[-2000:])
        viewer = pathlib.Path(built.stdout.strip().splitlines()[-1].removeprefix("built "))
        captured, sky = run(out / "sky", "--build-only")
        if captured.returncode:
            raise RuntimeError((captured.stdout + captured.stderr)[-2000:])
    return viewer, sky


def measure(out, specs, scenes):
    out = out.resolve()
    out.parent.mkdir(parents=True, exist_ok=True)
    readings = []
    for spec in specs:
        label, path = parse_capture(spec)
        readings.extend(labeled_capture(label, path))
    output = out.parent / f"{out.stem}-renders"
    output.mkdir(parents=True, exist_ok=True)
    scenes = tuple(dict.fromkeys(scenes))
    result = dict(revision=git(REPO, "rev-parse", "HEAD"), scenes={})
    with tempfile.TemporaryDirectory(prefix="scorecard-host-") as work:
        host = pathlib.Path(work)
        viewer, sky = build_hosts(host, scenes[0])
        with ProcessPoolExecutor(max_workers=len(scenes), mp_context=multiprocessing.get_context("spawn")) as pool:
            jobs = {key: pool.submit(measure_scene, key, readings, output, viewer, sky, host / key)
                    for key in scenes}
            result["scenes"] = {key: job.result() for key, job in jobs.items()}
    out.write_text(json.dumps(result, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    print(out)


def table(parent, tip, out, overlay=None, tip_tree=None):
    paths = (parent.resolve(), tip.resolve())
    revisions = [json.loads(path.read_text(encoding="utf-8")) for path in paths]
    if overlay and tip_tree is None:
        tip_tree = REPO
    if tip_tree is not None and git(tip_tree, "rev-parse", "HEAD") != revisions[1]["revision"]:
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
    scenes = args.scene or SCENES
    selection = [word for scene in scenes for word in ("--scene", scene)]

    def measured(arm, tree, sample_cache):
        subprocess.run([sys.executable, tree / "launcher/tools/bake/bake.py", "fetch",
                        *(f"launcher/demo/{scene}" for scene in scenes)], cwd=tree, check=True)
        out = args.out / f"{arm}.json"
        captures = [word for side, spec in args.capture if side == arm for word in ("--capture", spec)]
        subprocess.run([sys.executable, tree / ENTRY, "measure", "--out", out, *captures, *selection],
                       cwd=tree, check=True, env=dict(os.environ, **{SOURCE_SAMPLES_ENV: sample_cache}))
        return out

    args.out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="source-samples-", dir=args.out) as sample_cache:
        with ThreadPoolExecutor(max_workers=2) as pool:
            parent = pool.submit(measured, "parent", args.parent_tree, sample_cache)
            tip = pool.submit(measured, "tip", args.tip_tree, sample_cache)
            files = parent.result(), tip.result()
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
    tabling.add_argument("--tip-tree", type=pathlib.Path)
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
                  args.tip_tree.resolve() if args.tip_tree else None)
        elif args.command == "compare":
            compare(args)
        else:
            measure_pair(args)
    except (OSError, RuntimeError, ValueError, ImportError, AttributeError, subprocess.SubprocessError) as error:
        parser.exit(1, f"scorecard: {error}\n")


if __name__ == "__main__":
    main()
