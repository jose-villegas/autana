"""Rewrites a baked mesh's clusters and octree from the triangles and colours
it holds, without lighting the model again: what a change to the clustering or
the data format costs.

    python launcher/tools/r3d/rebake.py MESH_mesh_generated.c [options]

from the repository root. The mesh is rewritten in place unless --out-dir says
otherwise, and rewriting what was written is a fixed point: the same
triangles always give the same bytes. Anything before that stage, the model,
its simplification or its light, needs the import settings that baked it.
"""

import argparse
import os
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from r3d import log  # noqa: E402
from r3d.lit_mesh import finest_triangles, read_lit_mesh, write_lit_mesh  # noqa: E402

REPO = pathlib.Path(__file__).resolve().parents[3]
INTRO = "Its clusters and octree are rebuilt from the triangles and colours it holds, which were baked by:"
OPTIONS = (("--meshlet-triangles", "meshlet_triangles"), ("--leaf-triangles", "leaf_triangles"),
           ("--max-depth", "max_depth"))


def banner_of(path):
    """The lines of the generated file's opening comment."""
    text = pathlib.Path(path).read_text()
    body = text[text.index("/*") + 2 : text.index("*/")]
    return [line[3:] if line.startswith(" * ") else line.lstrip(" *") for line in body.strip("\n").split("\n")]


def provenance(lines):
    """What baked the triangles: the generator's own banner, past whatever an
    earlier rebake put above it."""
    start = lines.index(INTRO) + 1 if INTRO in lines else 1
    kept = lines[start:]
    while kept and not kept[0].strip():
        kept.pop(0)
    while kept and not kept[-1].strip():
        kept.pop()
    return kept


def command(out_file, options):
    """The one command that writes out_file again."""
    relative = pathlib.PurePath(os.path.relpath(out_file, REPO)).as_posix()
    text = f"python launcher/tools/r3d/rebake.py {relative}"
    for flag, key in OPTIONS:
        if key in options:
            text += f" {flag} {options[key]}"
    return text


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("mesh", help="a <name>_mesh_generated.c")
    parser.add_argument("--out-dir", help="where to write; the mesh's own directory when omitted")
    parser.add_argument("--meshlet-triangles", type=int, help="most triangles in a cluster")
    parser.add_argument("--leaf-triangles", type=int, help="most triangles an octree leaf holds")
    parser.add_argument("--max-depth", type=int, help="deepest octree")
    args = parser.parse_args(argv)
    options = {key: getattr(args, key) for _, key in OPTIONS if getattr(args, key) is not None}

    path = pathlib.Path(args.mesh)
    name = path.name[: -len("_mesh_generated.c")]
    out_dir = pathlib.Path(args.out_dir) if args.out_dir else path.parent
    mesh = read_lit_mesh(path)
    lines = banner_of(path)
    lines = ["GENERATED FILE - do not edit.", "", "    " + command(out_dir / path.name, options), "", INTRO, ""] \
        + provenance(lines)
    pos, rgb, tris, double, face = finest_triangles(mesh)
    baked = write_lit_mesh(out_dir, name, pos / mesh.position_scale, rgb, tris, double, lines,
                           position_scale=mesh.position_scale, face_rgb=face, **options)
    log(f"{name}: {len(baked.pos)} vertices, {len(baked.tris)} triangles, {len(baked.clusters)} clusters, "
        f"{len(baked.nodes)} nodes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
