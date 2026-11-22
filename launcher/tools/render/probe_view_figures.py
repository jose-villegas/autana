#!/usr/bin/env python3
"""Render a scene's baked SH probe view at two named camera samples."""
import argparse
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[3]
GFX_HEADER = ROOT / "launcher/main/gfx/gfx.h"
PROBES_HEADER = ROOT / "launcher/main/render/view/raster_probes.h"
sys.path.insert(0, str(ROOT / "launcher/tools"))
from build.c_header import read_define
from docs.figures import render_scene, save_png
from docs.producer import Producer

PRODUCER = Producer(
    area="render",
    needs=("host", "bakes"),
    outputs=("sponza-view-probes.png", "sponza-view-probes-close.png"),
    run=("--scene", "launcher/demo/sponza/sponza.scene.toml", "--object", "atrium",
         "--wide-camera", "camera", "--wide-frame", "20", "--wide-dt", "500",
         "--close-camera", "tour", "--close-frame", "36", "--close-dt", "1000", "--probe-size", "15",
         "--stem", "sponza-view-probes", "--out", "{out}"),
    reads=("launcher/demo/sponza/*",),
)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--scene", required=True, type=pathlib.Path)
    parser.add_argument("--object", required=True)
    for view in ("wide", "close"):
        parser.add_argument(f"--{view}-camera", required=True)
        parser.add_argument(f"--{view}-frame", required=True, type=int)
        parser.add_argument(f"--{view}-dt", required=True, type=int)
    parser.add_argument("--probe-size", type=int, default=read_define(PROBES_HEADER, "RASTER_PROBES_SIZE_DEFAULT"),
                        help="probe radius in hundredths of the grid spacing (probeview.size)")
    parser.add_argument("--stem", required=True)
    parser.add_argument("--out", required=True, type=pathlib.Path)
    parser.add_argument("--work", type=pathlib.Path, default=ROOT / "launcher/tools/results/render/probe_view")
    args = parser.parse_args()
    args.work.mkdir(parents=True, exist_ok=True)
    width, height = (read_define(GFX_HEADER, f"GFX_{axis}") for axis in ("WIDTH", "HEIGHT"))
    for view, suffix in (("wide", ""), ("close", "-close")):
        picture = args.work / f"{args.stem}{suffix}.bmp"
        render_scene(args.scene, "--object", args.object, "--view", "probes",
                     "--camera", getattr(args, f"{view}_camera"),
                     "--frames", getattr(args, f"{view}_frame"), "--dt", getattr(args, f"{view}_dt"),
                     "--quarter", 1, "--size", f"{width}x{height}", "--framegen", 0,
                     "--set", f"probeview.size={args.probe_size}", "-o", picture,
                     log=picture.with_suffix(".log"))
        save_png(picture, args.out / f"{args.stem}{suffix}.png")


if __name__ == "__main__":
    main()
