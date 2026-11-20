#!/usr/bin/env python3
"""Generate a white-skinned probe-view clip and stills along a scene's scripted path."""
import argparse
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[5]
sys.path.insert(0, str(ROOT / "launcher/tools"))
from docs.figures import gif_from_avi, save_png
from docs.producer import Producer

PRODUCER = Producer(
    area="render", needs=("host", "bakes"),
    outputs=("capybara-sponza-irradiance.gif", "capybara-sponza-irradiance-1.png",
             "capybara-sponza-irradiance-2.png"),
    run=("--scene", "capybara-sponza", "--duration-ms", "18500", "--stem", "capybara-sponza-irradiance",
         "--out", "{out}"),
    reads=("launcher/demo/capybara/*", "launcher/demo/sponza/*"),
)
BUILD_SCRIPT = ROOT / "launcher/main/apps/render_lab/tools/render_lab_render_host.sh"
DEFAULT_DT_MS = 100
MS_PER_SECOND = 1000
WHITE_ALBEDO = 1
CONSOLE_POSE_FRAMES = 2


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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--scene", required=True)
    parser.add_argument("--stem", required=True)
    parser.add_argument("--duration-ms", required=True, type=int)
    parser.add_argument("--dt-ms", type=int, default=DEFAULT_DT_MS)
    parser.add_argument("--times-ms", nargs=2, type=int)
    parser.add_argument("--out", type=pathlib.Path, required=True)
    parser.add_argument("--work", type=pathlib.Path, default=ROOT / "launcher/tools/results/render/irradiance")
    args = parser.parse_args()
    if args.duration_ms <= 0 or args.dt_ms <= 0:
        parser.error("duration and timestep must be positive")
    binary = renderer(args.work)
    movie = args.work / f"{args.stem}.avi"
    render(binary, args.scene, args.work / f"{args.stem}.bmp", "--dt", args.dt_ms,
           "--frames", (args.duration_ms + args.dt_ms - 1) // args.dt_ms, "--video", movie)
    args.out.mkdir(parents=True, exist_ok=True)
    gif_from_avi(movie, args.out / f"{args.stem}.gif", MS_PER_SECOND / args.dt_ms)
    for index, time in enumerate(args.times_ms or pose_times(args.duration_ms), 1):
        picture = args.work / f"{args.stem}-{index}.bmp"
        render(binary, args.scene, picture, "--console", f"0:pose {time}", "--frames", CONSOLE_POSE_FRAMES)
        save_png(picture, args.out / f"{args.stem}-{index}.png")


if __name__ == "__main__":
    main()
