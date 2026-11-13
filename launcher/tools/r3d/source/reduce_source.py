"""Reduce a large model to an OBJ source an import can read.

    python launcher/tools/r3d/source/reduce_source.py IN OUT_DIR --name NAME --error E \\
        [--error-for MATERIALS=E ...] [--texture-size PX] [--objects PATTERN] [--source-up Z|Y] [--blender PATH]

Blender exports IN's meshes and base-colour textures (blend_export_obj.py,
which reads its options), then meshoptimizer's simplifier removes every
vertex whose removal moves the surface less than E source units, and every
separate piece smaller than that, nothing that moves it more. Each object is
simplified on its own and written as its own `o` group, so the source keeps
its objects. Each object's parts (its triangles joined through shared
vertices) are matched as rigid copies (objects/instances.py) across every
object and simplified once: each later copy is its first part's result moved
by the copy's transform, so an object whose parts all repeat another's stays
that object's copy and the import finds it again. Within one, where a texture coordinate or a material changes,
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
import fnmatch
import pathlib
import shutil
import subprocess
import sys
import tempfile

import numpy as np

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2]))

from r3d.meshopt import ERROR_ABSOLUTE, PRUNE, simplify_to_error  # noqa: E402
from r3d.obj import load_obj  # noqa: E402
from r3d.geometry import bounds_diagonal, grouped_indices
from r3d.objects.object_error import part_labels, part_names
from r3d.objects.instances import MATCH_TOLERANCE, match_instances, place, source_objects  # noqa: E402

HERE = pathlib.Path(__file__).resolve().parent


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
    return simplify_to_error(positions, triangles, error, ERROR_ABSOLUTE | PRUNE)


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


def part_ranks(shapes, index, owner, tolerance):
    """Each part's rank within its object: by shape, then by its place in the frame of the object's largest part,
    so objects assembled alike list their triangles alike and the import matches them whole."""
    placed = {index[copy.object_name]: (number, copy) for number, shape in enumerate(shapes)
              for copy in shape.instances}
    anchors = {}
    for part, (number, copy) in sorted(placed.items()):
        size = len(shapes[number].triangles)
        if size > anchors.get(owner[part], (0, None))[0]:
            anchors[owner[part]] = (size, copy)
    keys = []
    for part in range(len(owner)):
        number, copy = placed[part]
        anchor = anchors[owner[part]][1]
        local = (copy.position - anchor.position) @ anchor.linear
        keys.append((int(owner[part]), number, *np.rint(local / tolerance).tolist()))
    ranks = np.empty(len(owner), dtype=np.int64)
    ranks[sorted(range(len(owner)), key=keys.__getitem__)] = np.arange(len(owner))
    return ranks


def place_copies(shapes, index, positions, uvs, kept, materials, parts):
    """Each later rigid copy of a shape as its first part's kept triangles, moved by the
    copy's transform (and rewound when it mirrors), so the copies stay exact and the import
    finds them again."""
    columns = [(positions, uvs, kept, materials, parts)]
    base = len(positions)
    by_part = grouped_indices(parts)
    for shape in shapes:
        first = shape.instances[0]
        mine = by_part.get(index[first.object_name], np.zeros(0, dtype=np.int64))
        corners, faces = np.unique(kept[mine], return_inverse=True)
        faces = faces.reshape(-1, 3)
        local = positions[corners] - first.position
        for copy in shape.instances[1:]:
            placed = faces[:, ::-1] if copy.mirrored else faces
            columns.append((place(local, copy.linear, copy.position), uvs[corners], placed + base, materials[mine],
                            np.full(len(faces), index[copy.object_name])))
            base += len(corners)
    return tuple(np.concatenate(column) for column in zip(*columns))


def reduce(exported, name, out, error, error_for=()):
    """Simplifies the exported NAME.obj in `exported` object by object into
    `out`, beside its .mtl and textures; returns the triangle counts before
    and after."""
    obj_path = exported / f"{name}.obj"
    p, uv, tri_v, tri_t, tri_m, names, tri_o, object_names = load_obj(str(obj_path))
    if len(uv) == 0:
        uv = np.zeros((1, 2))
    tri_p, _ = part_labels(p, tri_v, tri_o)
    names_of_parts, owner, _ = part_names(tri_p, tri_o, object_names)
    # One vertex per (position, texture coordinate, material, part) corner:
    # a change in the first three is a seam the simplifier keeps on both
    # sides, and no vertex is shared between parts.
    corners = np.stack([tri_v.reshape(-1), tri_t.reshape(-1), np.repeat(tri_m, 3), np.repeat(tri_p, 3)], axis=1)
    unique, inverse = np.unique(corners, axis=0, return_inverse=True)
    triangles = inverse.reshape(-1, 3)
    material_error = material_errors(names, error, error_for)
    positions, uvs = p[unique[:, 0]], uv[unique[:, 1]]
    shapes = match_instances(source_objects(p, tri_v, tri_m, tri_p, names_of_parts))
    index = {name: i for i, name in enumerate(names_of_parts)}
    firsts = {index[shape.instances[0].object_name] for shape in shapes}
    passes, which = np.unique(np.stack([tri_p, np.searchsorted(np.unique(material_error), material_error[tri_m])],
                                       axis=1), axis=0, return_inverse=True)
    rows = grouped_indices(which.reshape(-1))
    kept = np.concatenate([simplify(positions, triangles[rows[k]], material_error[tri_m[rows[k][0]]])
                           for k, (part, _) in enumerate(passes) if part in firsts])
    # Each kept triangle's material and part are its corners' own.
    materials, parts = unique[kept[:, 0], 2], unique[kept[:, 0], 3]
    positions, uvs, kept, materials, parts = place_copies(shapes, index, positions, uvs, kept, materials, parts)
    order = np.lexsort((np.arange(len(kept)), part_ranks(shapes, index, owner, MATCH_TOLERANCE * bounds_diagonal(p))[parts]))
    kept, materials, objects = kept[order], materials[order], owner[parts[order]]
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
