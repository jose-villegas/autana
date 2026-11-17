"""The copies of a carved start bundle in the fit: drawn as constants beside the rest, so the rest is fitted
against the same picture the board draws, and the copy-coverage gate on what the fit made of the rest.

A copy mesh is local and colourless; each copy row places it under its own transform with its own colour
set (mesh_import.write_shape). The fit draws every placed copy, frozen: copies are never moved, recoloured
or pruned.
"""

import pathlib
import tempfile

import numpy as np

from r3d import lcol_asset, mesh_bundle
from r3d.fit.gbuffer import IN_FRONT, distance, reference_buffers
from r3d.lit_mesh import read_lit_mesh, rgb888

# The copy-coverage gate: over the held-out poses, the fitted rest may hide more of the frame's copy pixels
# than its start by at most HIDDEN_MEAN_GROWTH on the mean and HIDDEN_WORST_GROWTH on the worst pose. Set
# just above what the fit with frozen copies hides (at most +0.06 points mean, +0.86 worst pose), so a fit
# that hides copies as the rest-only fit did (+0.10, +1.40) fails; a fit that hides fewer lowers them.
HIDDEN_MEAN_GROWTH = 0.0007
HIDDEN_WORST_GROWTH = 0.010


def placed_copies(bundle, flat):
    """Every copy in a start bundle placed under its transform, as one FitMesh in the rest's units: welded
    per copy mesh, one colour per vertex, or per triangle when `flat`. None when the bundle has no copies."""
    from r3d.appearance_simplify import FitMesh

    entries, rows = mesh_bundle.decode(pathlib.Path(bundle).read_bytes())
    if not rows:
        return None
    data = {name: blob for name, _kind, blob in entries}
    points, rgb, tris, double, vertex_point = [], [], [], [], []
    point_count = vertex_count = 0
    for name in dict.fromkeys(row["mesh"] for row in rows):
        with tempfile.TemporaryDirectory() as folder:
            path = pathlib.Path(folder) / f"{name}.mesh"
            path.write_bytes(data[name])
            mesh = read_lit_mesh(path)
        sided = np.zeros(len(mesh.tris), dtype=np.int64)
        for _vb, _vc, tbase, tcount, _lo, _hi, is_double in mesh.clusters:
            sided[tbase:tbase + tcount] = int(is_double)
        local, welded = np.unique(mesh.pos, axis=0, return_inverse=True)
        local, welded = local / float(mesh.position_scale), welded.reshape(-1)
        encoding, _count, sets = lcol_asset.decode(data[lcol_asset.name_for_mesh(name)])
        if (encoding == lcol_asset.RGB565) != flat:
            raise ValueError(f"copy mesh {name} is {'flat' if encoding == lcol_asset.RGB565 else 'smooth'} "
                             f"and its rest is not")
        for row in (row for row in rows if row["mesh"] == name):
            colours = sets[row["colour_set"]]
            colours = rgb888(np.frombuffer(colours, "<u2")) if flat else np.frombuffer(colours, "u1").reshape(-1, 3)
            points.append(local @ np.asarray(row["matrix"], dtype=np.float64).T + np.asarray(row["position"]))
            rgb.append(colours / 255.0)
            tris.append(mesh.tris + vertex_count)
            double.append(sided)
            vertex_point.append(welded + point_count)
            point_count += len(local)
            vertex_count += len(welded)
    return FitMesh(np.concatenate(points), np.concatenate(rgb), np.concatenate(tris), np.concatenate(double), 1,
                   np.concatenate(vertex_point), flat)


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
