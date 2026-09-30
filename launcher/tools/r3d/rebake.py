"""Rewrites a baked mesh in the current format: the finest triangles, with
their colours, are read back from its generated C and clustered, levelled and
emitted again. A change to the cluster format then costs no relight.

    python -m r3d.rebake IN_mesh_generated.c --out-dir DIR [options]

Run from launcher/tools. The geometry and colours are the input's own, so
only what the bake stage after lighting does can change.
"""

import argparse
import pathlib
import re
import sys

from r3d import log
from r3d.lit_mesh import finest_triangles, read_lit_mesh, write_lit_mesh


def banner_of(path):
    """The lines of the generated file's opening comment."""
    text = pathlib.Path(path).read_text()
    body = text[text.index("/*") + 2 : text.index("*/")]
    return [re.sub(r"^ \* ?", "", line).rstrip() for line in body.strip("\n").split("\n")]


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("mesh", help="a <name>_mesh_generated.c, in any format this bake has written")
    parser.add_argument("--out-dir", required=True)
    parser.add_argument("--leaf-triangles", type=int, default=320, help="most triangles an octree leaf holds")
    parser.add_argument("--max-depth", type=int, default=10)
    parser.add_argument("--meshlet-triangles", type=int, default=64)
    parser.add_argument("--partition-size", type=int, default=8, help="meshlets merged into one group")
    parser.add_argument("--colour-weight", type=float, default=1.0)
    args = parser.parse_args(argv)

    path = pathlib.Path(args.mesh)
    name = path.name[: -len("_mesh_generated.c")]
    mesh = read_lit_mesh(str(path))
    pos, rgb, tris, double = finest_triangles(mesh)
    lines = banner_of(path)
    cut = next((i for i, line in enumerate(lines) if line.startswith("Rebaked")), len(lines))
    lines = lines[:cut]
    while lines and not lines[-1]:
        lines.pop()
    lines += [
        "",
        "Rebaked with r3d.rebake: the geometry and colours are the bake's own; the",
        "meshlets, coarser levels and octree were rebuilt from them:",
        f"    python -m r3d.rebake {path.name} --out-dir . --leaf-triangles {args.leaf_triangles}"
        f" --max-depth {args.max_depth} \\",
        f"        --meshlet-triangles {args.meshlet_triangles} --partition-size {args.partition_size}"
        f" --colour-weight {args.colour_weight:g}",
    ]
    baked = write_lit_mesh(args.out_dir, name, pos / mesh.position_scale, rgb, tris, double, lines,
                           args.leaf_triangles, args.max_depth, mesh.position_scale,
                           meshlet_triangles=args.meshlet_triangles, partition_size=args.partition_size,
                           colour_weight=args.colour_weight)
    coarse = baked.lod.cluster_count if baked.lod else 0
    log(f"{name}: {len(baked.pos)} vertices, {len(baked.tris)} triangles, {len(baked.clusters)} clusters, "
        f"{len(baked.nodes)} nodes, {coarse} coarser clusters")


if __name__ == "__main__":
    sys.exit(main())
