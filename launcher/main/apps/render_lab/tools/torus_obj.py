"""Writes a torus about the y axis as an OBJ, faces counter-clockwise from outside, with a material
of the file's own name. Tessellated finely, its baked cluster tree has several levels.

    python main/apps/render_lab/tools/torus_obj.py OUT.obj RING_RADIUS TUBE_RADIUS RING_STEPS TUBE_STEPS

from launcher/. The radii are in scene units; each (ring step, tube step) pair is one vertex and two
triangles. Standard library only.
"""

import argparse
import math
import pathlib

DECIMALS = 5


def torus_obj(material, command, ring_radius, tube_radius, ring_steps, tube_steps):
    """The torus as OBJ text, under a banner naming `command`."""
    lines = ["# GENERATED FILE - do not edit.", "#", f"#     {command}", "#", f"mtllib {material}.mtl"]
    for i in range(ring_steps):
        a = 2.0 * math.pi * i / ring_steps
        for j in range(tube_steps):
            b = 2.0 * math.pi * j / tube_steps
            r = ring_radius + tube_radius * math.cos(b)
            x, y, z = r * math.cos(a), tube_radius * math.sin(b), -r * math.sin(a)
            lines.append(f"v {x:.{DECIMALS}f} {y:.{DECIMALS}f} {z:.{DECIMALS}f}")
    lines.append(f"usemtl {material}")

    def vertex(i, j):
        return (i % ring_steps) * tube_steps + (j % tube_steps) + 1

    for i in range(ring_steps):
        for j in range(tube_steps):
            a, b, c, d = vertex(i, j), vertex(i + 1, j), vertex(i + 1, j + 1), vertex(i, j + 1)
            lines += [f"f {a} {b} {c}", f"f {a} {c} {d}"]
    return "\n".join(lines) + "\n"


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
    args.out.write_text(torus_obj(args.out.stem, command, args.ring_radius, args.tube_radius, args.ring_steps,
                                  args.tube_steps), encoding="utf-8", newline="\n")


if __name__ == "__main__":
    main()
