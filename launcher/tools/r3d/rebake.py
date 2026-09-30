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
from r3d.lit_mesh import emit, finest_triangles, migrate, read_lit_mesh, write_lit_mesh


def banner_of(path):
    """The lines of the generated file's opening comment."""
    text = pathlib.Path(path).read_text()
    body = text[text.index("/*") + 2 : text.index("*/")]
    return [re.sub(r"^ \* ?", "", line).rstrip() for line in body.strip("\n").split("\n")]


def command(args):
    """The options after --out-dir that repeat this run."""
    if args.clustering == "keep":
        return ""
    text = f" --clustering {args.clustering} --leaf-triangles {args.leaf_triangles} --max-depth {args.max_depth}"
    if args.clustering == "meshlet":
        text += (f" --meshlet-triangles {args.meshlet_triangles} --partition-size {args.partition_size}"
                 f" --colour-weight {args.colour_weight:g}" + (" --lod" if args.lod else ""))
    return text


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("mesh", help="a <name>_mesh_generated.c, in any format this bake has written")
    parser.add_argument("--out-dir", required=True)
    parser.add_argument("--clustering", choices=("keep", "octree", "meshlet"), default="keep",
                        help="keep: the clusters and tree as they are; octree: each octree leaf is a cluster; "
                        "meshlet: compact clusters under an octree")
    parser.add_argument("--leaf-triangles", type=int, help="most triangles an octree leaf holds (160, or 320 for meshlets)")
    parser.add_argument("--max-depth", type=int, default=10)
    parser.add_argument("--meshlet-triangles", type=int, default=64)
    parser.add_argument("--partition-size", type=int, default=8, help="meshlets merged into one group")
    parser.add_argument("--colour-weight", type=float, default=1.0)
    parser.add_argument("--lod", action="store_true", help="also emit the coarser levels (default: finest only)")
    args = parser.parse_args(argv)
    if args.leaf_triangles is None:
        args.leaf_triangles = 160 if args.clustering == "octree" else 320

    path = pathlib.Path(args.mesh)
    name = path.name[: -len("_mesh_generated.c")]
    mesh = read_lit_mesh(str(path))
    lines = banner_of(path)
    cut = next((i for i, line in enumerate(lines) if line.startswith("Rebaked")), len(lines))
    lines = lines[:cut]
    while lines and not lines[-1]:
        lines.pop()
    lines += [
        "",
        "Rebaked with r3d.rebake: the geometry and colours are the bake's own; the",
        ("clusters and tree are as baked" if args.clustering == "keep" else "clusters and octree were rebuilt from them")
        + (", coarser levels added:" if args.lod else ":"),
        f"    python -m r3d.rebake {path.name} --out-dir ." + command(args),
    ]
    if args.clustering == "keep":
        assert not args.lod, "levels need meshlet clusters"
        baked = migrate(mesh)
        emit(args.out_dir, name, lines, baked)
        log(f"{name}: {len(baked.pos)} vertices, {len(baked.tris)} triangles, {len(baked.clusters)} clusters, "
            f"{len(baked.nodes)} nodes")
        return 0
    pos, rgb, tris, double = finest_triangles(mesh)
    baked = write_lit_mesh(args.out_dir, name, pos / mesh.position_scale, rgb, tris, double, lines,
                           args.leaf_triangles, args.max_depth, mesh.position_scale,
                           clustering=args.clustering, meshlet_triangles=args.meshlet_triangles, partition_size=args.partition_size,
                           colour_weight=args.colour_weight, with_lod=args.lod)
    coarse = baked.lod.cluster_count if baked.lod else 0
    log(f"{name}: {len(baked.pos)} vertices, {len(baked.tris)} triangles, {len(baked.clusters)} clusters, "
        f"{len(baked.nodes)} nodes, {coarse} coarser clusters")


if __name__ == "__main__":
    sys.exit(main())
