#!/usr/bin/env python3
"""Compare the torch appearance forward model with the nvdiffrast library.

    sh launcher/tools/r3d/gpu_python.sh launcher/tools/r3d/appearance_parity.py \
        --start NAME.mesh --poses POSES.txt [--poses POSES.txt] --scale 2
"""

import argparse
import pathlib
import sys

import torch

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from r3d.appearance_simplify import Renderer, deterministic_fit, pose_views, start_mesh

PIXEL_TOLERANCE = 1.0 / 255.0


def reference_image(renderer, points, colours, matrix):
    """Public library operations, kept outside the fit's gradient path."""
    clip = renderer.clip(points, matrix)[None].contiguous()
    tris = renderer.tris[renderer.facing(clip[0])].contiguous()
    width, height = renderer.size
    rast, _ = renderer.dr.rasterize(renderer.context, clip, tris, resolution=[height, width])
    attributes = colours.repeat_interleave(3, dim=0) if renderer.flat else colours
    image, _ = renderer.dr.interpolate(attributes[None].contiguous(), rast, tris)
    image = torch.where(rast[..., 3:] > 0, image, renderer.clear)
    return renderer.dr.antialias(image.contiguous(), rast, clip, tris)[0].flip(0)


@deterministic_fit()
def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--start", required=True)
    parser.add_argument("--poses", action="append", required=True)
    parser.add_argument("--scale", type=int, default=2)
    parser.add_argument("--clear", type=float, nargs=3, default=(0, 0, 0))
    args = parser.parse_args(argv)
    mesh = start_mesh(args.start)
    points = torch.as_tensor(mesh.points, dtype=torch.float32, device="cuda")
    colours = torch.as_tensor(mesh.rgb, dtype=torch.float32, device="cuda")
    total, count, maximum, changed = 0.0, 0, 0.0, 0
    with torch.no_grad():
        for poses in args.poses:
            views, size = pose_views(poses, args.scale)
            renderer = Renderer(mesh.tris, mesh.double, mesh.vertex_point, size, "cuda", args.clear, mesh.flat)
            for index, (matrix, *_rest) in enumerate(views):
                matrix = torch.as_tensor(matrix, dtype=torch.float32, device="cuda")
                difference = (renderer(points, colours, matrix) - reference_image(renderer, points, colours, matrix)).abs()
                peak, mean = float(difference.max()), float(difference.mean())
                pixels = int((difference.amax(-1) > PIXEL_TOLERANCE).sum())
                print(f"{poses}:{index}: max={peak:.8f} mean={mean:.8f} pixels>1/255={pixels}")
                total += float(difference.double().sum())
                count += difference.numel()
                maximum = max(maximum, peak)
                changed += pixels
    print(f"all: max={maximum:.8f} mean={total / max(count, 1):.8f} pixels>1/255={changed}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
