"""Reduce a large model to an OBJ source an import can read.

    python launcher/tools/r3d/source/reduce_source.py IN OUT_DIR --name NAME --error E \\
        [--error-for MATERIALS=E ...] [--texture-size PX] [--objects PATTERN] [--source-up Z|Y] [--blender PATH]

Blender exports IN's meshes and base-colour textures (blend_export_obj.py,
which reads its options), then meshoptimizer's simplifier removes every
vertex whose removal moves the surface less than E source units, and every
separate piece smaller than that, nothing that moves it more. Each object is
simplified on its own and written as its own `o` group, so the source keeps
its objects. Rigid copies (objects/instances.py) are simplified once: each
later copy is its first object's result moved by the copy's transform, so the
import finds them as copies again. Within one, where a texture coordinate or a material changes,
the vertices on either side stay a seam that both sides keep, so no crack
opens. --error-for gives the materials whose names match a glob their own
error, in a pass of their own, for a material such as alpha-tested cards that
the import thins or masks anyway. Writes OUT_DIR/NAME.obj, its .mtl and
OUT_DIR/textures/, and prints the triangle count before and after.

A source too large to commit, or to trace in a bake, is reduced once
with this and the result committed beside its import file with the original's
credit; the import's own `simplify` then brings it to each variant's budget.
"""
import argparse
import ctypes
import fnmatch
import pathlib
import shutil
import subprocess
import sys
import tempfile

import numpy as np

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2]))

from r3d.meshopt import _library  # noqa: E402
from r3d.obj import load_obj  # noqa: E402
from r3d.objects.instances import match_instances, place, source_objects  # noqa: E402

HERE = pathlib.Path(__file__).resolve().parent
# meshoptimizer.h's meshopt_SimplifyErrorAbsolute (the error is in model units)
# and meshopt_SimplifyPrune (a piece smaller than the error goes entirely).
ERROR_ABSOLUTE = 1 << 2
PRUNE = 1 << 3


def arguments():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("source")
    parser.add_argument("out_dir")
    parser.add_argument("--name", required=True, help="the OBJ's file name, without extension")
    parser.add_argument("--error", type=float, required=True, help="the largest surface move, in source units")
    parser.add_argument("--error-for", action="append", default=[], metavar="MATERIALS=E",
                        help="simplify materials matching the glob MATERIALS on their own, to error E")
    parser.add_argument("--texture-size", default="512", help="the longest texture side, in pixels")
    parser.add_argument("--objects", default="*", help="keep mesh objects whose names match this glob")
    parser.add_argument("--source-up", default="Z", help="the up axis as Blender read the source")
    parser.add_argument("--blender", default="blender", help="the Blender to export with")
    return parser.parse_args()


def export(args, directory):
    subprocess.run([args.blender, "--background", "--factory-startup", "--python", str(HERE / "blend_export_obj.py"),
                    "--", str(pathlib.Path(args.source).resolve()), str(directory), "--name", args.name,
                    "--texture-size", args.texture_size, "--objects", args.objects, "--source-up", args.source_up],
                   check=True)


def simplify(positions, triangles, error):
    """The triangles that remain once no vertex or piece can go without moving
    the surface more than `error`."""
    lib = _library()
    fn = lib.meshopt_simplify
    fn.restype = ctypes.c_size_t
    p = np.ascontiguousarray(positions, dtype=np.float32)
    i = np.ascontiguousarray(triangles.reshape(-1), dtype=np.uint32)
    out = np.empty_like(i)
    c = ctypes.c_void_p
    n = fn(out.ctypes.data_as(c), i.ctypes.data_as(c), ctypes.c_size_t(len(i)), p.ctypes.data_as(c),
           ctypes.c_size_t(len(p)), ctypes.c_size_t(12), ctypes.c_size_t(0), ctypes.c_float(error),
           ctypes.c_uint(ERROR_ABSOLUTE | PRUNE), None)
    return out[:n].reshape(-1, 3).astype(np.int64)


def write_obj(path, mtllib, positions, uvs, triangles, objects, object_names, materials, names):
    used = np.unique(triangles)
    remap = np.full(len(positions), -1, dtype=np.int64)
    remap[used] = np.arange(len(used))
    lines = [f"mtllib {mtllib}"]
    lines += [f"v {x:.3f} {y:.3f} {z:.3f}" for x, y, z in positions[used]]
    lines += [f"vt {u:.5f} {v:.5f}" for u, v in uvs[used]]
    for obj in np.unique(objects):
        lines.append(f"o {object_names[obj]}")
        for material in np.unique(materials[objects == obj]):
            lines.append(f"usemtl {names[material]}")
            for a, b, c in remap[triangles[(objects == obj) & (materials == material)]] + 1:
                lines.append(f"f {a}/{a} {b}/{b} {c}/{c}")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def material_errors(names, error, error_for):
    """Each material's error: `error`, or the last MATERIALS=E rule whose glob matches it."""
    errors = np.full(len(names), float(error))
    for rule in error_for:
        pattern, _, value = rule.rpartition("=")
        errors[[i for i, name in enumerate(names) if fnmatch.fnmatchcase(name, pattern)]] = float(value)
    return errors


def place_copies(shapes, index, positions, uvs, kept, materials, objects):
    """Each later rigid copy of a shape as its first object's kept triangles, moved by the
    copy's transform (and rewound when it mirrors), so the copies stay exact and the import
    finds them again."""
    parts = [(positions, uvs, kept, materials, objects)]
    base = len(positions)
    for shape in shapes:
        first = shape.instances[0]
        mine = objects == index[first.object_name]
        corners, faces = np.unique(kept[mine], return_inverse=True)
        faces = faces.reshape(-1, 3)
        local = positions[corners] - first.position
        for copy in shape.instances[1:]:
            placed = faces[:, ::-1] if copy.mirrored else faces
            parts.append((place(local, copy.linear, copy.position), uvs[corners], placed + base, materials[mine],
                          np.full(len(faces), index[copy.object_name])))
            base += len(corners)
    return tuple(np.concatenate(column) for column in zip(*parts))


def reduce(exported, name, out, error, error_for=()):
    """Simplifies the exported NAME.obj in `exported` object by object into
    `out`, beside its .mtl and textures; returns the triangle counts before
    and after."""
    obj_path = exported / f"{name}.obj"
    p, uv, tri_v, tri_t, tri_m, names, tri_o, object_names = load_obj(str(obj_path))
    if len(uv) == 0:
        uv = np.zeros((1, 2))
    # One vertex per (position, texture coordinate, material, object) corner:
    # a change in the first three is a seam the simplifier keeps on both
    # sides, and no vertex is shared between objects.
    corners = np.stack([tri_v.reshape(-1), tri_t.reshape(-1), np.repeat(tri_m, 3), np.repeat(tri_o, 3)], axis=1)
    unique, inverse = np.unique(corners, axis=0, return_inverse=True)
    triangles = inverse.reshape(-1, 3)
    errors = material_errors(names, error, error_for)[tri_m]
    positions, uvs = p[unique[:, 0]], uv[unique[:, 1]]
    shapes = match_instances(source_objects(p, tri_v, tri_m, tri_o, object_names))
    index = {name: i for i, name in enumerate(object_names)}
    firsts = {index[shape.instances[0].object_name] for shape in shapes}
    kept = np.concatenate([simplify(positions, triangles[(tri_o == obj) & (errors == e)], e)
                           for obj, e in np.unique(np.stack([tri_o, errors], axis=1), axis=0) if obj in firsts])
    # Each kept triangle's material and object are its corners' own.
    materials, objects = unique[kept[:, 0], 2], unique[kept[:, 0], 3]
    positions, uvs, kept, materials, objects = place_copies(shapes, index, positions, uvs, kept, materials, objects)
    out.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(exported / f"{name}.mtl", out / f"{name}.mtl")
    if (exported / "textures").is_dir():
        shutil.copytree(exported / "textures", out / "textures", dirs_exist_ok=True)
    write_obj(out / f"{name}.obj", f"{name}.mtl", positions, uvs, kept, objects, object_names,
              materials, names)
    return len(tri_v), len(kept)


def main():
    args = arguments()
    with tempfile.TemporaryDirectory() as temporary:
        exported = pathlib.Path(temporary)
        export(args, exported)
        before, after = reduce(exported, args.name, pathlib.Path(args.out_dir), args.error, args.error_for)
    print(f"reduce_source: {before} triangles to {after}")


if __name__ == "__main__":
    main()
