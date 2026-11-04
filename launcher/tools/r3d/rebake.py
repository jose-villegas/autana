"""Rewrites a baked mesh's clusters and octree from the triangles and colours
it holds, without lighting the model again: what a change to the clustering or
the data format costs.

    python launcher/tools/r3d/rebake.py MESH.bundle [options]

from the repository root. The mesh is rewritten in place unless --out-dir says
otherwise, and rewriting what was written is a fixed point: the same
triangles always give the same bytes. Anything before that stage, the model,
its simplification or its light, needs the import settings that baked it.
Run build_pack.py afterwards to put the result in the pack.
"""

import argparse
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from r3d import log, mesh_bundle, mesh_asset, lod_asset
from r3d.lod.bake_levels import bake_lit_mesh  # noqa: E402
from r3d.import_settings import load_import_settings  # noqa: E402
from r3d.lit_mesh import finest_triangles  # noqa: E402

OPTIONS = ("meshlet_triangles", "leaf_triangles", "max_depth")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("mesh", help="a <name>.bundle")
    parser.add_argument("--import", dest="settings", help="geometry settings for clustering")
    parser.add_argument("--out-dir", help="where to write; the mesh's own directory when omitted")
    parser.add_argument("--meshlet-triangles", type=int, help="most triangles in a cluster")
    parser.add_argument("--leaf-triangles", type=int, help="most triangles an octree leaf holds")
    parser.add_argument("--max-depth", type=int, help="deepest octree")
    args = parser.parse_args(argv)
    options = {key: getattr(args, key) for key in OPTIONS if getattr(args, key) is not None}

    if args.settings:
        settings = load_import_settings(args.settings)
        options.setdefault("meshlet_triangles", settings.meshlet_triangles)
        options.setdefault("lod", settings.lod)

    path = pathlib.Path(args.mesh)
    out_dir = pathlib.Path(args.out_dir) if args.out_dir else path.parent
    entries, copies = mesh_bundle.decode(path.read_bytes())
    products = []
    for name, kind, blob in entries:
        if kind != mesh_asset.TYPE:
            if kind != lod_asset.TYPE:
                products.append((name, kind, blob))
            continue
        mesh = mesh_bundle.read_mesh(entries, name)
        pos, rgb, tris, double, face = finest_triangles(mesh)
        baked = bake_lit_mesh(pos / mesh.position_scale, rgb, tris, double,
                              position_scale=mesh.position_scale, face_rgb=face,
                              **{ "lod": mesh.lod is not None, **options})
        products.extend(mesh_bundle.mesh_entries(name, baked))
        log(f"{name}: {len(baked.pos)} vertices, {len(baked.tris)} triangles, {len(baked.clusters)} clusters, "
            f"{len(baked.nodes)} nodes")
    (out_dir / path.name).write_bytes(mesh_bundle.encode(products, copies))
    return 0


if __name__ == "__main__":
    sys.exit(main())
