"""Place and recolour a carved bundle's unmasked copies, and gate how much the fitted rest hides them.

Copies keep their geometry and transforms. Placements of the same mesh and LCOL set share colours.
"""

import pathlib
from typing import NamedTuple

import numpy as np

from r3d import lcol_asset, mesh_bundle
from r3d.texture import texture_asset, uv_transfer
from r3d.fit.gbuffer import IN_FRONT, distance, reference_buffers
from r3d.lit_mesh import finest, panel_colour, read_bundle_mesh, rgb888

# Colours are fitted in unit sRGB and written in byte units before any RGB565 packing.
STORED_CHANNEL_MAX = np.iinfo(np.uint8).max

# The copy-coverage gate: over the held-out poses, the fitted rest may hide more of the frame's copy pixels
# than its start by at most HIDDEN_MEAN_GROWTH on the mean and HIDDEN_WORST_GROWTH on the worst pose. Set
# just above what the fit with frozen copies hides (at most +0.06 points mean, +0.86 worst pose), so a fit
# that hides copies as the rest-only fit did (+0.10, +1.40) fails; a fit that hides fewer lowers them.
HIDDEN_MEAN_GROWTH = 0.0007
HIDDEN_WORST_GROWTH = 0.010


def drawn_copies(entries, rows):
    """The copy rows the fit draws. A masked shape (cards) is left out: the fit's references hide the material it
    replaces (reference_render.lit_source), so the fit sees the surfaces it is later drawn over."""
    masked = masked_copies(entries, rows)
    return [row for row in rows if row not in masked]


def masked_copies(entries, rows):
    """Copy rows whose shape draws MASK1 pages; their material match records the hidden source material."""
    data = {name: (kind, blob) for name, kind, blob in entries}

    def masked(mesh):
        row = data.get(uv_transfer.name_for_mesh(mesh))
        if row is None or row[0] != uv_transfer.TYPE:
            return False
        luvs = uv_transfer.decode(row[1])
        return any(texture_asset.decode(data[uv_transfer.texture_name(mesh, int(page))][1]).format == texture_asset.MASK1
                   for page in range(luvs.material_count))
    return [row for row in rows if masked(row["mesh"])]


def hidden_materials(entries, rows):
    """Source material names hidden by decoded bundle card shapes, without selecting or baking cards."""
    return {row["match"]["material"] for row in masked_copies(entries, rows)}


class CopyColours(NamedTuple):
    """Finest stored RGB rows, the indices used by placed geometry and the slice for each (mesh, set)."""
    rgb: np.ndarray
    placed: np.ndarray
    sets: dict[tuple[str, int], slice]


def placed_copies(bundle, flat):
    """The unmasked copies in a bundle as a FitMesh, or None."""
    entries, rows = mesh_bundle.decode(pathlib.Path(bundle).read_bytes())
    return place_copies(entries, rows, flat)[0]


def _finest_rows(mesh):
    """Stored vertex and face indices of the finest clusters, with one sidedness flag per face."""
    shown = [cluster for cluster, level0 in zip(mesh.clusters, finest(mesh)) if level0]
    vertices = np.concatenate([np.arange(vbase, vbase + vcount) for vbase, vcount, *_ in shown])
    faces = np.concatenate([np.arange(tbase, tbase + tcount) for _, _, tbase, tcount, *_ in shown])
    sided = np.concatenate([np.full(tcount, int(is_double), dtype=np.int64)
                            for _, _, _, tcount, _, _, is_double in shown])
    return vertices, faces, sided


def place_copies(entries, rows, flat):
    """Place the finest clusters under their row transforms and return their tied colour parameters."""
    from r3d.appearance_simplify import FitMesh

    rows = drawn_copies(entries, rows)
    if not rows:
        return None, None
    data = {name: blob for name, _kind, blob in entries}
    points, tris, double, vertex_point = [], [], [], []
    rgb, placed, slices = [], [], {}
    point_count = vertex_count = colour_count = 0
    for name in dict.fromkeys(row["mesh"] for row in rows):
        mesh = read_bundle_mesh(entries, name)
        vertices, faces, sided = _finest_rows(mesh)
        renumber = np.full(len(mesh.pos), -1, dtype=np.int64)
        renumber[vertices] = np.arange(len(vertices))
        local, welded = np.unique(mesh.pos[vertices], axis=0, return_inverse=True)
        local, welded = local / float(mesh.position_scale), welded.reshape(-1)
        encoding, _count, sets = lcol_asset.decode(data[lcol_asset.name_for_mesh(name)])
        if (encoding == lcol_asset.RGB565) != flat:
            raise ValueError(f"copy mesh {name} is {'flat' if encoding == lcol_asset.RGB565 else 'smooth'} "
                             f"and its rest is not")
        for row in (row for row in rows if row["mesh"] == name):
            key = name, row["colour_set"]
            if key not in slices:
                payload = sets[row["colour_set"]]
                channels = lcol_asset.STRIDES[lcol_asset.RGB888]
                colours = (rgb888(np.frombuffer(payload, "<u2")[faces]) if flat
                           else np.frombuffer(payload, "u1").reshape(-1, channels)[vertices])
                slices[key] = slice(colour_count, colour_count + len(colours))
                colour_count += len(colours)
                rgb.append(colours / STORED_CHANNEL_MAX)
            span = slices[key]
            points.append(local @ np.asarray(row["matrix"], dtype=np.float64).T + np.asarray(row["position"]))
            placed.append(np.arange(span.start, span.stop))
            tris.append(renumber[mesh.tris[faces]] + vertex_count)
            double.append(sided)
            vertex_point.append(welded + point_count)
            point_count += len(local)
            vertex_count += len(welded)
    colours = CopyColours(np.concatenate(rgb), np.concatenate(placed), slices)
    return (FitMesh(np.concatenate(points), colours.rgb[colours.placed], np.concatenate(tris),
                    np.concatenate(double), 1, np.concatenate(vertex_point), flat), colours)


def write_colours(entries, slices, trained):
    """Return the changed LCOL entries, with coarse rows taking their nearest finest row's colour."""
    from scipy.spatial import cKDTree

    data = {name: blob for name, _kind, blob in entries}
    replacements = []
    for mesh_name in dict.fromkeys(mesh for mesh, _ in slices):
        key = lcol_asset.name_for_mesh(mesh_name)
        encoding, count, sets = lcol_asset.decode(data[key])
        mesh = read_bundle_mesh(entries, mesh_name)
        vertices, faces, _ = _finest_rows(mesh)
        flat = encoding == lcol_asset.RGB565
        fine = faces if flat else vertices
        coarse = np.setdiff1d(np.arange(count), fine)
        source = np.empty(count, dtype=np.int64)
        source[fine] = np.arange(len(fine))
        if len(coarse):
            # Three times the face centroid keeps the nearest search in exact quantised coordinates.
            positions = mesh.pos[mesh.tris].sum(axis=1) if flat else mesh.pos
            source[coarse] = cKDTree(positions[fine]).query(positions[coarse])[1]
        for i in range(len(sets)):
            span = slices.get((mesh_name, i))
            if span is not None:
                rgb = np.clip(np.rint(trained[span] * STORED_CHANNEL_MAX), 0, STORED_CHANNEL_MAX)
                stored = panel_colour(rgb) if flat else rgb.astype("u1")
                sets[i] = stored[source].tobytes()
        replacements.append((key, lcol_asset.TYPE, lcol_asset.encode(encoding, count, sets)))
    return replacements

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
