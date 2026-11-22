#!/usr/bin/env python3
"""Generate a white-skinned probe-view clip and stills along a scene's scripted path."""
import argparse
import json
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[5]
sys.path.insert(0, str(ROOT / "launcher/tools"))
from docs.figures import gif_from_avi, save_png
from render.check_avi import read_video
from PIL import Image
from r3d.skin_reference import pose_frame
from r3d.mitsuba_reference import DEFAULT_SPP
from irradiance_sheet import compose
from docs.producer import Producer

PRODUCER = Producer(
    area="render", needs=("host", "bakes"),
    outputs=("capybara-sponza-irradiance.gif", "capybara-sponza-irradiance-1.png",
             "capybara-sponza-irradiance-2.png", "capybara-sponza-irradiance-3.png",
             "capybara-sponza-irradiance-sheet.png"),
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


def render(binary, scene, output, *flags, view="probes"):
    """Run the host with the same white-albedo override in every view."""
    with output.with_suffix(".log").open("w") as log:
        subprocess.run([str(binary), "--quarter", "1", "--no-hud", "--scene", scene, "--view", view,
                        "--framegen", "0", "--set", f"skin.albedo={WHITE_ALBEDO}",
                        *map(str, flags), "-o", str(output)], cwd=ROOT, stdout=log, stderr=subprocess.STDOUT,
                       check=True)
    text = output.with_suffix(".log").read_text(errors="replace")
    if "RENDER_ERR" in text or "TUNE_ERR" in text:
        raise RuntimeError(f"host refused settings: {output.with_suffix('.log')}")


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
    written = set()
    _, pictures = read_video(movie)
    for index, picture in enumerate(pictures):
        for folder, frame in zip(folders, selected):
            if index == frame["frame"]:
                Image.fromarray(picture).save(folder / "ours.bmp")
                written.add(index)
    if any(frame["frame"] not in written for frame in selected):
        raise ValueError("pose JSON frame is absent from host video")
    return folders


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
    movie = args.work / f"{args.stem}.avi"
    poses = args.work / f"{args.stem}.json"
    render(binary, args.scene, args.work / f"{args.stem}.bmp", "--pose-json", poses, "--dt", args.dt_ms,
           "--frames", (args.duration_ms + args.dt_ms - 1) // args.dt_ms, "--video", movie)
    args.out.mkdir(parents=True, exist_ok=True)
    gif_from_avi(movie, args.out / f"{args.stem}.gif", MS_PER_SECOND / args.dt_ms)
    folders = matched_stills(movie, poses, args.times_ms or pose_times(args.duration_ms), args.dt_ms,
                             args.entity, args.work)
    for index, folder in enumerate(folders, 1):
        reference(folder, args.reference_scene, args.glb, args.entity, args.spp, args.variant)
        save_png(folder / "ours.bmp", args.out / f"{args.stem}-{index}.png")
    stats = compose(folders, args.out / f"{args.stem}-sheet.png")
    (args.work / "measurements.json").write_text(json.dumps(stats, indent=2) + "\n")


if __name__ == "__main__":
    main()
