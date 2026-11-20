"""Writes a torus about the y axis as an OBJ, faces counter-clockwise from outside, with a material
of the file's own name. Fine tessellation is what gives the bake a deep cluster tree.

    python main/apps/render_lab/tools/torus_obj.py OUT.obj RING_RADIUS TUBE_RADIUS RING_STEPS TUBE_STEPS

from launcher/. The radii are in scene units; each (ring step, tube step) pair is one vertex and two
triangles.
"""

import argparse
import math
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[4] / "tools"))
from r3d.obj import write_obj  # noqa: E402

DECIMALS = 5


def torus(ring_radius, tube_radius, ring_steps, tube_steps):
    """The torus's positions and its 0-based triangles."""
    positions = []
    for i in range(ring_steps):
        a = 2.0 * math.pi * i / ring_steps
        for j in range(tube_steps):
            b = 2.0 * math.pi * j / tube_steps
            r = ring_radius + tube_radius * math.cos(b)
            positions.append((r * math.cos(a), tube_radius * math.sin(b), -r * math.sin(a)))

    def vertex(i, j):
        return (i % ring_steps) * tube_steps + (j % tube_steps)

    triangles = []
    for i in range(ring_steps):
        for j in range(tube_steps):
            a, b, c, d = vertex(i, j), vertex(i + 1, j), vertex(i + 1, j + 1), vertex(i, j + 1)
            triangles += [(a, b, c), (a, c, d)]
    return positions, triangles


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("out", type=pathlib.Path)
    parser.add_argument("ring_radius", type=float)
    parser.add_argument("tube_radius", type=float)
    parser.add_argument("ring_steps", type=int)
    parser.add_argument("tube_steps", type=int)
    args = parser.parse_args()
    command = (f"python main/apps/render_lab/tools/torus_obj.py {args.out.as_posix()} "
               f"{args.ring_radius:g} {args.tube_radius:g} {args.ring_steps} {args.tube_steps}")
    positions, triangles = torus(args.ring_radius, args.tube_radius, args.ring_steps, args.tube_steps)
    material = args.out.stem
    write_obj(args.out, f"{material}.mtl", positions, [(None, material, triangles)],
              banner=("GENERATED FILE - do not edit.", "", f"    {command}", ""), position_decimals=DECIMALS)


if __name__ == "__main__":
    main()
