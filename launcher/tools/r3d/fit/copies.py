"""The copies of a carved start bundle in the fit: drawn beside the rest, so the rest is fitted against the
same picture the board draws, trained with it, and the copy-coverage gate on what the fit made of the rest.

A copy mesh is local and colourless; each copy row places it under its own transform with its own colour
set (mesh_import.write_shape). The fit trains each copy's colour set and holds every copy where the start
placed it: a trained shape moved slabs of the rest over copies it hid. Copies are never pruned. write_copies
rebuilds the copy meshes with the fit's colours.
"""

import pathlib

import numpy as np

from r3d import lcol_asset, mesh_bundle
from r3d.fit.gbuffer import IN_FRONT, distance, reference_buffers
from r3d.lit_mesh import finest, rgb888

# The copy-coverage gate: over the held-out poses, the fitted rest may hide more of the frame's copy pixels
# than its start by at most HIDDEN_MEAN_GROWTH on the mean and HIDDEN_WORST_GROWTH on the worst pose. Set
# just above what the fit with frozen copies hides (at most +0.06 points mean, +0.86 worst pose), so a fit
# that hides copies as the rest-only fit did (+0.10, +1.40) fails; a fit that hides fewer lowers them.
HIDDEN_MEAN_GROWTH = 0.0007
HIDDEN_WORST_GROWTH = 0.010
# The bundle the fit writes its copies into, beside the fitted rest.
COPIES_FILE = "copies.bundle"


def _copy_meshes(entries, rows):
    """Per copy mesh of a bundle, in its rows' order: its name, mesh, finest clusters' vertices and faces, each
    face's sidedness, the renumbering of those vertices from 0, and their positions welded (local, in the rest's
    units, and each vertex's row of it)."""
    for name in dict.fromkeys(row["mesh"] for row in rows):
        mesh = mesh_bundle.read_mesh(entries, name)
        shown = [c for c, level0 in zip(mesh.clusters, finest(mesh)) if level0]
        vertices = np.concatenate([np.arange(vbase, vbase + vcount) for vbase, vcount, *_ in shown])
        faces = np.concatenate([np.arange(tbase, tbase + tcount) for _, _, tbase, tcount, *_ in shown])
        sided = np.concatenate([np.full(tcount, int(is_double), dtype=np.int64)
                                for _, _, _, tcount, _, _, is_double in shown])
        renumber = np.full(len(mesh.pos), -1, dtype=np.int64)
        renumber[vertices] = np.arange(len(vertices))
        local, welded = np.unique(mesh.pos[vertices], axis=0, return_inverse=True)
        yield name, mesh, vertices, faces, sided, renumber, local / float(mesh.position_scale), welded.reshape(-1)


def placed_copies(bundle, flat):
    """Every copy in a start bundle placed under its transform, as one FitMesh in the rest's units: a copy
    mesh's finest clusters as the bundle stores them, their own vertices with positions welded, one colour per
    vertex, or per triangle when `flat`. None when the bundle has no copies. No weld by value, so a copy's
    vertices and triangles are the ones a mesh without levels holds."""
    from r3d.appearance_simplify import FitMesh

    entries, rows = mesh_bundle.decode(pathlib.Path(bundle).read_bytes())
    if not rows:
        return None
    data = {name: blob for name, _kind, blob in entries}
    points, rgb, tris, double, vertex_point = [], [], [], [], []
    point_count = vertex_count = 0
    for name, mesh, vertices, faces, sided, renumber, local, welded in _copy_meshes(entries, rows):
        encoding, _count, sets = lcol_asset.decode(data[lcol_asset.name_for_mesh(name)])
        if (encoding == lcol_asset.RGB565) != flat:
            raise ValueError(f"copy mesh {name} is {'flat' if encoding == lcol_asset.RGB565 else 'smooth'} "
                             f"and its rest is not")
        for row in (row for row in rows if row["mesh"] == name):
            colours = sets[row["colour_set"]]
            colours = rgb888(np.frombuffer(colours, "<u2"))[faces] if flat                 else np.frombuffer(colours, "u1").reshape(-1, 3)[vertices]
            linear, offset = np.asarray(row["matrix"], dtype=np.float64), np.asarray(row["position"], dtype=np.float64)
            points.append(local @ linear.T + offset)
            rgb.append(colours / 255.0)
            tris.append(renumber[mesh.tris[faces]] + vertex_count)
            double.append(sided)
            vertex_point.append(welded + point_count)
            point_count += len(local)
            vertex_count += len(welded)
    return FitMesh(np.concatenate(points), np.concatenate(rgb), np.concatenate(tris), np.concatenate(double), 1,
                   np.concatenate(vertex_point), flat)


def write_copies(start, copies, job, out_dir, recorder=None):
    """The start bundle's copy meshes rebuilt with the colours the fit trained on `copies` (placed_copies'
    FitMesh) into out_dir/COPIES_FILE, with the start's copy rows: each copy mesh's shape as the start holds it,
    its copies' colour sets from their trained colours."""
    from r3d.mesh_import import copy_mesh_entries

    entries, rows = mesh_bundle.decode(pathlib.Path(start).read_bytes())
    out, colour_at = [], 0
    for name, mesh, vertices, faces, sided, renumber, local, welded in _copy_meshes(entries, rows):
        corners = renumber[mesh.tris[faces]]
        positions = local[welded[corners]].reshape(-1, 3)
        sets = {}
        for row in (row for row in rows if row["mesh"] == name):
            count = len(faces) if copies.flat else len(vertices)
            block = np.clip(np.rint(copies.rgb[colour_at:colour_at + count] * 255.0), 0, 255)
            colour_at += count
            sets[row["colour_set"]] = block if copies.flat else block[corners].reshape(-1, 3)
        keys = np.concatenate([sets[k] for k in sorted(sets)], axis=1)
        out += copy_mesh_entries(job, name, positions, keys, sided, {"position_scale": mesh.position_scale},
                                 recorder)
    path = pathlib.Path(out_dir) / COPIES_FILE
    path.write_bytes(mesh_bundle.encode(out, rows))
    return path


def joined(rest, copies):
    """The rest and the copies as one mesh to raster, the rest's triangles first."""
    if copies is None:
        return rest
    return rest._replace(points=np.concatenate([rest.points, copies.points]),
                         rgb=np.concatenate([rest.rgb, copies.rgb]),
                         tris=np.concatenate([rest.tris, copies.tris + len(rest.vertex_point)]),
                         double=np.concatenate([np.asarray(rest.double), copies.double]),
                         vertex_point=np.concatenate([rest.vertex_point, copies.vertex_point + len(rest.points)]))


def hidden_copy_pixels(source, carved, meshes, poses_path, device="cuda"):
    """For each mesh of `meshes`, per pose of `poses_path`: the fraction of the frame where the source shows
    a carved triangle (`carved`, one flag per source triangle) and the mesh stands in front of it."""
    import torch

    from r3d.appearance_simplify import Renderer, projection
    from r3d.poses import read_poses

    width, height, lens, near, poses = read_poses(poses_path)
    as_points = lambda p: torch.as_tensor(p, dtype=torch.float32, device=device)
    matrices = [as_points(projection(pose[:3], pose[3:], width, height, lens, near)) for pose in poses]
    buffers = reference_buffers(source, carved, matrices, [(width, height)] * len(poses), device)
    renders = [(Renderer(m.tris, m.double, m.vertex_point, (width, height), device, flat=m.flat), as_points(m.points))
               for m in meshes]
    hidden = np.zeros((len(meshes), len(poses)))
    with torch.no_grad():
        for i, (matrix, reference, copy) in enumerate(zip(matrices, *buffers)):
            for k, (render, points) in enumerate(renders):
                fitted, _shown = distance(render, points, matrix)
                hidden[k, i] = float((copy & (fitted < reference * (1 - IN_FRONT))).float().mean())
    return hidden


def check_hidden_copies(start, fitted):
    """Raise when the fitted rest hides more copy pixels than its start beyond the gate's tolerances; `start`
    and `fitted` are hidden_copy_pixels rows. Returns the log line."""
    mean, worst = fitted.mean() - start.mean(), fitted.max() - start.max()
    line = (f"hidden copy pixels: start {start.mean():.2%} mean, {start.max():.2%} worst pose; "
            f"fitted {fitted.mean():.2%}, {fitted.max():.2%}")
    if mean > HIDDEN_MEAN_GROWTH or worst > HIDDEN_WORST_GROWTH:
        raise ValueError(f"the fitted rest hides the copies: {line} (growth allowed {HIDDEN_MEAN_GROWTH:.2%} mean, "
                         f"{HIDDEN_WORST_GROWTH:.2%} worst pose)")
    return line
