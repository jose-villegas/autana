#!/usr/bin/env python3
"""Generate white-skinned probe lighting figures along a scene's scripted path."""
import argparse
import json
import os
import pathlib
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[5]
sys.path.insert(0, str(ROOT / "launcher/tools"))
import numpy as np
from asset.asset_pack import build_pack, parse_pack
from asset.engine_frame import to_engine
from bake import bake, produce
from docs.figures import gif_from_avi
from render.check_avi import read_video
from PIL import Image
from r3d import sh_probe_asset as shpr, sh_probe_bake as sh
from r3d.build_pack import PACK_SUFFIX, pack_jobs
from r3d.skin_reference import pose_frame
from r3d.mitsuba_reference import DEFAULT_SPP
from irradiance_sheet import PROBE_TERMS, compose
from docs.producer import Producer

PRODUCER = Producer(
    area="render", needs=("host", "bakes"),
    outputs=("capybara-sponza-irradiance.gif", "capybara-sponza-irradiance-sheet.png"),
    run=("--scene", "capybara-sponza", "--duration-ms", "18500", "--stem", "capybara-sponza-irradiance",
         "--reference-scene", "launcher/demo/sponza/sponza.scene.toml",
         "--glb-bake", "capybara.glb", "--entity", "capybara", "--times-ms", "3000", "6200", "12000",
         "--out", "{out}"),
    reads=("launcher/demo/capybara/*", "launcher/demo/sponza/*"),
)
BUILD_SCRIPT = ROOT / "launcher/main/apps/render_lab/tools/render_lab_render_host.sh"
DEFAULT_DT_MS = 100
MS_PER_SECOND = 1000
WHITE_ALBEDO = 1
REFERENCE_SCRIPT = ROOT / "launcher/tools/r3d/skin_reference.py"


def renderer(work):
    """Build the app's shared renderer, with its existing asset setup."""
    work.mkdir(parents=True, exist_ok=True)
    with (work / "build.log").open("w") as log:
        subprocess.run(["sh", str(BUILD_SCRIPT), "--build-only", "-o", str(work)],
                       cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, check=True)
    return work / ("render_lab_render.exe" if sys.platform == "win32" else "render_lab_render")


def render(binary, scene, output, *flags, view="probes", assets=None):
    """Run the host with the same white-albedo override in every view."""
    env = dict(os.environ, AUTANA_ASSET_DIR=str(assets.resolve())) if assets is not None else None
    with output.with_suffix(".log").open("w") as log:
        subprocess.run([str(binary), "--quarter", "1", "--no-hud", "--scene", scene, "--view", view,
                        "--framegen", "0", "--set", f"skin.albedo={WHITE_ALBEDO}",
                        *map(str, flags), "-o", str(output)], cwd=ROOT, stdout=log, stderr=subprocess.STDOUT,
                       check=True, env=env)
    text = output.with_suffix(".log").read_text(errors="replace")
    if "RENDER_ERR" in text or "TUNE_ERR" in text:
        raise RuntimeError(f"host refused settings: {output.with_suffix('.log')}")


def split_probes(job, scene, total):
    """Replay the bake's direct lights and return two SHPR records in engine coordinates."""
    settings, camera = job.settings, scene.camera.component
    source = sh.load_source(settings)
    alpha = sh.AlphaTest(source, settings.alpha_keep) if settings.alpha_keep is not None else None
    query = sh.RayQuery(source.p, source.tri_v, hit_filter=alpha)
    points, cell, origin, step, dims = sh.grid(source, camera, sh.load(scene.path).spacing, settings.seed)
    moved, state = sh.classify(query, sh.face_normals(source.p, source.tri_v)[0], points, cell, camera.near_z)
    outside = state != 'inside'
    coefficients = np.zeros((len(points), shpr.COEFFICIENTS, shpr.CHANNELS))
    coefficients[outside] = sh.direct_term(query, moved[outside], sh.baked_lights(scene.lights),
                                           job.bake.ray_offset)[0]
    inside = (~outside).reshape(tuple(dims))
    sun = sh.dilate(coefficients.reshape(*dims, shpr.COEFFICIENTS, shpr.CHANNELS), inside)
    sun_data = to_engine(shpr.TYPE, shpr.encode(origin, step, dims, sun.transpose(2, 1, 0, 3, 4),
                                                inside.transpose(2, 1, 0)))
    sun_grid = shpr.decode(sun_data)
    origin, step, dims, _, coefficients, inside = total
    if sun_grid[:3] != total[:3] or not np.array_equal(sun_grid[5], inside):
        raise ValueError("replayed sun lattice differs from the shipped probe lattice")
    return sun_data, shpr.encode(origin, step, dims, coefficients - sun_grid[4], inside)


def split_assets(assets, scene_path, work):
    """Copy the built asset directory for each term and replace only its SHPR entry."""
    scene_path = scene_path.resolve()
    packs, jobs = pack_jobs([scene_path])
    probe, = (item for item in bake.bakes_in(packs, jobs) if item.kind == 'sh_probes')
    pack_name, = (name for name, entries in packs.items() if scene_path in entries.values())
    pack_file = pack_name + PACK_SUFFIX
    entries = parse_pack((assets / pack_file).read_bytes())
    kind, data = entries[probe.output]
    if kind != shpr.TYPE:
        raise ValueError(f"{probe.output} is not a probe grid")
    job = probe.job
    if bake.is_blend(job.settings):
        job = produce.with_source(job, produce.export_file(job.settings, bake.read_lock(), bake.default_cache()))
    split = split_probes(job, probe.scene, shpr.decode(data))
    copies = {}
    for term, record in zip(PROBE_TERMS, split):
        destination = work / term / assets.name
        shutil.copytree(assets, destination, dirs_exist_ok=True)
        entries[probe.output] = (kind, record)
        (destination / pack_file).write_bytes(build_pack([(name, *entry) for name, entry in entries.items()]))
        copies[term] = destination
    return copies, probe.scene.tonemap_white


def pose_times(duration_ms):
    """Samples a quarter and three quarters through a rest-to-rest run."""
    return [duration_ms // 4, duration_ms * 3 // 4]


def matched_stills(movie, poses, times_ms, dt_ms, entity, work):
    """Extract actual host frames nearest the requested times, restricted to complete blends."""
    frames = json.loads(poses.read_text())
    candidates = []
    for frame in frames:
        try:
            pose_frame([frame], entity=entity)
        except ValueError:
            continue
        candidates.append(frame)
    if not candidates:
        raise ValueError("no host frames have a complete skin blend and camera")
    selected = [min(candidates, key=lambda item: abs(item["frame"] * dt_ms - time)) for time in times_ms]
    if len({frame["frame"] for frame in selected}) != len(selected):
        raise ValueError("requested stills select the same complete host frame")
    folders = [work / f"pose-{index}" for index in range(1, len(selected) + 1)]
    for folder, frame in zip(folders, selected):
        folder.mkdir(parents=True, exist_ok=True)
        (folder / "pose.json").write_text(json.dumps([frame], indent=2) + "\n")
    extract_stills(movie, folders, "ours")
    return folders


def extract_stills(movie, folders, stem):
    """Use the total run's frame indices for every lighting term."""
    frames = {json.loads((folder / "pose.json").read_text())[0]["frame"]: folder for folder in folders}
    _, pictures = read_video(movie)
    for index, picture in enumerate(pictures):
        if index in frames:
            Image.fromarray(picture).save(frames.pop(index) / f"{stem}.bmp")
        if not frames:
            return
    raise ValueError("pose JSON frame is absent from host video")


def reference(folder, scene, glb, entity, spp, variant=None):
    """Trace one exported pose without reconstructing the app's motion logic."""
    with Image.open(folder / "ours.bmp") as picture:
        size = picture.size
    command = [sys.executable, str(REFERENCE_SCRIPT), "--scene", str(scene), "--glb", str(glb),
               "--pose-json", str(folder / "pose.json"), "--entity", entity, "--white",
               "--size", *map(str, size), "--spp", str(spp), "--out", str(folder)]
    if variant:
        command += ["--variant", variant]
    with (folder / "reference.log").open("w") as log:
        subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--scene", required=True)
    parser.add_argument("--stem", required=True)
    parser.add_argument("--reference-scene", required=True, type=pathlib.Path)
    model = parser.add_mutually_exclusive_group(required=True)
    model.add_argument("--glb", type=pathlib.Path)
    model.add_argument("--glb-bake")
    parser.add_argument("--entity", required=True)
    parser.add_argument("--spp", type=int, default=DEFAULT_SPP)
    parser.add_argument("--variant")
    parser.add_argument("--duration-ms", required=True, type=int)
    parser.add_argument("--dt-ms", type=int, default=DEFAULT_DT_MS)
    parser.add_argument("--times-ms", nargs="+", type=int)
    parser.add_argument("--out", type=pathlib.Path, required=True)
    parser.add_argument("--work", type=pathlib.Path, default=ROOT / "launcher/tools/results/render/irradiance")
    args = parser.parse_args()
    if args.duration_ms <= 0 or args.dt_ms <= 0:
        parser.error("duration and timestep must be positive")
    if args.glb_bake:
        args.glb = pathlib.Path(subprocess.check_output(
            [sys.executable, str(ROOT / "launcher/tools/bake/bake.py"), "path", args.glb_bake],
            cwd=ROOT, text=True).strip())
    binary = renderer(args.work)
    assets = binary.parent / "assets"
    copies, tonemap_white = split_assets(assets, args.reference_scene, args.work)
    movie = args.work / f"{args.stem}.avi"
    poses = args.work / f"{args.stem}.json"
    timing = ("--dt", args.dt_ms, "--frames", (args.duration_ms + args.dt_ms - 1) // args.dt_ms)
    render(binary, args.scene, movie.with_suffix(".bmp"), "--pose-json", poses, *timing, "--video", movie,
           view="shaded", assets=assets)
    args.out.mkdir(parents=True, exist_ok=True)
    gif_from_avi(movie, args.out / f"{args.stem}.gif", MS_PER_SECOND / args.dt_ms)
    folders = matched_stills(movie, poses, args.times_ms or pose_times(args.duration_ms), args.dt_ms,
                             args.entity, args.work)
    for term, asset_dir in copies.items():
        term_movie = asset_dir.parent / f"{args.stem}.avi"
        render(binary, args.scene, term_movie.with_suffix(".bmp"), *timing, "--video", term_movie,
               view="shaded", assets=asset_dir)
        extract_stills(term_movie, folders, term)
    for folder in folders:
        reference(folder, args.reference_scene, args.glb, args.entity, args.spp, args.variant)
    stats = compose(folders, args.out / f"{args.stem}-sheet.png", tonemap_white=tonemap_white)
    (args.work / "measurements.json").write_text(json.dumps(stats, indent=2) + "\n")


if __name__ == "__main__":
    main()
