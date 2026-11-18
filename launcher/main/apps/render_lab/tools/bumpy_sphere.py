#!/usr/bin/env python3
"""Write a bumpy, colour-banded sphere as an OBJ and its MTL beside it.

    python launcher/main/apps/render_lab/tools/bumpy_sphere.py OUT.obj

Writes OUT.obj and OUT.mtl: a UV sphere of RINGS rings
and twice as many slices, its radius pushed out by BUMP_OCTAVES of
sinusoidal bumps so no two neighbourhoods simplify alike, and banded into
BANDS materials from bottom to top so its levels carry colour steps.
Deterministic: run again, the files come out byte for byte the same.
"""

import argparse
import math
import pathlib

RINGS = 40
# Each octave doubles the bumps' frequency and halves their height.
BUMP_OCTAVES = 3
BUMP_HEIGHT = 0.08
BUMP_FREQUENCY = 5
BANDS = 4
# Band colours, bottom to top: the stress scene's low and high band ends and two between.
BAND_COLOURS = ((0.16, 0.31, 0.55), (0.42, 0.45, 0.48), (0.69, 0.58, 0.41), (0.91, 0.69, 0.35))


def radius(theta, phi):
    """1 plus the octaves of bumps at polar angle `theta` and azimuth `phi`."""
    bump = 0.0
    for octave in range(BUMP_OCTAVES):
        frequency = BUMP_FREQUENCY * 2 ** octave
        bump += BUMP_HEIGHT / 2 ** octave * math.sin(frequency * theta + octave) * math.cos(frequency * phi)
    return 1.0 + bump


def ball():
    """(vertices, triangles by band): poles, then RINGS - 1 rings of 2 x RINGS vertices."""
    slices = 2 * RINGS
    vertices = [(0.0, radius(0.0, 0.0), 0.0)]
    for ring in range(1, RINGS):
        theta = math.pi * ring / RINGS
        for s in range(slices):
            phi = 2 * math.pi * s / slices
            r = radius(theta, phi)
            vertices.append((r * math.sin(theta) * math.cos(phi), r * math.cos(theta), r * math.sin(theta) * math.sin(phi)))
    vertices.append((0.0, -radius(math.pi, 0.0), 0.0))
    south = len(vertices)
    bands = [[] for _ in range(BANDS)]

    def at(ring, s):
        return 2 + (ring - 1) * slices + s % slices

    def band(ring):
        return min(BANDS - 1, (RINGS - ring) * BANDS // RINGS)

    for s in range(slices):
        bands[band(0)].append((1, at(1, s + 1), at(1, s)))
        bands[band(RINGS - 1)].append((south, at(RINGS - 1, s), at(RINGS - 1, s + 1)))
        for ring in range(1, RINGS - 1):
            a, b, c, d = at(ring, s), at(ring, s + 1), at(ring + 1, s + 1), at(ring + 1, s)
            bands[band(ring)] += [(a, b, c), (a, c, d)]
    return vertices, bands


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("out", type=pathlib.Path, help="the OBJ to write; its MTL goes beside it")
    out = parser.parse_args().out
    mtl = out.with_suffix(".mtl")
    vertices, bands = ball()
    lines = ["# A bumpy sphere, written by launcher/main/apps/render_lab/tools/bumpy_sphere.py.",
             f"mtllib {mtl.name}", f"o {out.stem}"]
    lines += [f"v {x:.6f} {y:.6f} {z:.6f}" for x, y, z in vertices]
    for index, triangles in enumerate(bands):
        lines.append(f"usemtl band_{index}")
        lines += [f"f {a} {b} {c}" for a, b, c in triangles]
    out.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    materials = []
    for index, (r, g, b) in enumerate(BAND_COLOURS):
        materials += [f"newmtl band_{index}", f"Kd {r} {g} {b}"]
    mtl.write_text("\n".join(materials) + "\n", encoding="utf-8", newline="\n")
    print(f"bumpy_sphere: {len(vertices)} vertices, {sum(map(len, bands))} triangles")


if __name__ == "__main__":
    main()
