#!/usr/bin/env python3
"""A linear model of what a baked mesh costs to draw from a pose, its
weights fitted to board frame times.

    python launcher/tools/r3d/cost_model.py features MESH_mesh_generated.c --poses POSES
    python launcher/tools/r3d/cost_model.py fit TABLE
    python launcher/tools/r3d/cost_model.py predict MESH_mesh_generated.c --poses POSES --weights WEIGHTS

TABLE has one `ms feature...` row per measured frame, and fit prints the
WEIGHTS predict reads. POSES are at the board's render size.

    ms = constant + per_submitted * triangles of the clusters in view
         + per_drawn * drawn triangles + per_row * triangle rows
         + per_pixel * covered pixels + per_cluster * clusters in view

A cluster is in view unless all its box lies beyond one clip plane, and its
triangles are then submitted: fetched, transformed and tested. A drawn triangle is in front of the near plane, faces the camera (or is
double-sided) and overlaps the screen; its rows and pixels are its height
and area on screen, clipped to the screen by its bounding box. Pixels count
overdraw: every triangle's own area, before the depth test. The features
work on NumPy arrays and on PyTorch tensors alike, so the same model is a
differentiable penalty during a fit.
"""

import pathlib
import sys

import numpy as np

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

FEATURES = ("constant", "submitted", "drawn", "rows", "pixels", "clusters")


def _clamp(xp, value, low=None, high=None):
    return np.clip(value, low, high) if xp is np else value.clamp(min=low, max=high)


def triangle_terms(xp, clip, tris, double, width, height):
    """(drawn, rows, pixels) per triangle for clip-space positions `clip`
    (N x 4, y up) and `xp` NumPy or PyTorch; drawn is boolean, rows and
    pixels are zero where a triangle is not drawn."""
    corners = clip[tris]
    w = corners[..., 3]
    safe = xp.where(w > 1e-6, w, w * 0 + 1e-6)
    x = (corners[..., 0] / safe + 1) * (0.5 * width)
    y = (1 - corners[..., 1] / safe) * (0.5 * height)
    signed = (x[:, 1] - x[:, 0]) * (y[:, 2] - y[:, 0]) - (x[:, 2] - x[:, 0]) * (y[:, 1] - y[:, 0])
    lo_x, hi_x, lo_y, hi_y = xp.amin(x, 1), xp.amax(x, 1), xp.amin(y, 1), xp.amax(y, 1)
    seen_w = _clamp(xp, _clamp(xp, hi_x, high=float(width)) - _clamp(xp, lo_x, low=0.0), low=0.0)
    seen_h = _clamp(xp, _clamp(xp, hi_y, high=float(height)) - _clamp(xp, lo_y, low=0.0), low=0.0)
    box = _clamp(xp, (hi_x - lo_x) * (hi_y - lo_y), low=1e-9)
    drawn = (w > 0).all(1) & ((signed < 0) | double) & (seen_w > 0) & (seen_h > 0)
    zero = signed * 0
    pixels = xp.where(drawn, abs(signed) * 0.5 * (seen_w * seen_h) / box, zero)
    rows = xp.where(drawn, seen_h, zero)
    return drawn, rows, pixels


def clusters_in_view(boxes, clip_matrix):
    """Which axis-aligned boxes (lo, hi rows of a K x 2 x 3 array) the view
    frustum of `clip_matrix` can touch: a box is out only when all its
    corners lie beyond one clip plane."""
    lo, hi = boxes[:, 0], boxes[:, 1]
    corners = np.stack([np.where(np.array([i >> k & 1 for k in range(3)], dtype=bool), hi, lo) for i in range(8)], axis=1)
    clip = np.concatenate([corners, np.ones(corners.shape[:2] + (1,))], axis=2) @ clip_matrix.T
    x, y, w = clip[..., 0], clip[..., 1], clip[..., 3]
    outside = (x > w).all(1) | (x < -w).all(1) | (y > w).all(1) | (y < -w).all(1) | (w <= 0).all(1)
    return ~outside


def features(submitted, drawn, rows, pixels, clusters):
    """The model's feature row, in FEATURES order, from one pose's counts and
    per-triangle terms."""
    return np.array([1.0, float(submitted), float(drawn.sum()), float(rows.sum()), float(pixels.sum()), float(clusters)])


def fit(rows, ms):
    """Non-negative weights, one per FEATURES entry, by least squares."""
    from scipy.optimize import nnls

    rows = np.asarray(rows, dtype=float)
    scale = np.maximum(rows.max(axis=0), 1e-12)
    weights, _ = nnls(rows / scale, np.asarray(ms, dtype=float))
    return weights / scale


def predict(weights, rows):
    return np.asarray(rows, dtype=float) @ np.asarray(weights, dtype=float)


def mesh_rows(path, poses_path):
    """One feature row per pose of a poses file for a generated mesh."""
    from r3d.appearance_simplify import projection
    from r3d.lit_mesh import finest_triangles, read_lit_mesh
    from r3d.poses import read_poses

    mesh = read_lit_mesh(path)
    q, _rgb, tris, double, _face = finest_triangles(mesh)
    positions = q / float(mesh.position_scale)
    boxes = np.array([[lo, hi] for _, _, _, _, lo, hi, _ in mesh.clusters], dtype=float) / mesh.position_scale
    sizes = np.array([count for _, _, _, count, _, _, _ in mesh.clusters])
    width, height, lens, near, poses = read_poses(poses_path)
    rows = []
    for pose in poses:
        matrix = projection(pose[:3], pose[3:], width, height, lens, near)
        clip = np.concatenate([positions, np.ones((len(positions), 1))], axis=1) @ matrix.T
        drawn, heights, pixels = triangle_terms(np, clip, tris, double.astype(bool), width, height)
        seen = clusters_in_view(boxes, matrix)
        rows.append(features(sizes[seen].sum(), drawn, heights, pixels, seen.sum()))
    return np.array(rows)


def main(argv=None):
    import argparse

    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("command", choices=("features", "fit", "predict"))
    parser.add_argument("path", help="a generated mesh, or for fit the table")
    parser.add_argument("--poses", help="a sample_tracks.sh poses file at the board's render size")
    parser.add_argument("--weights", help="the weights fit printed")
    args = parser.parse_args(argv)
    if args.command == "fit":
        table = np.loadtxt(args.path, ndmin=2)
        print(" ".join("%.9g" % value for value in fit(table[:, 1:], table[:, 0])))
        return 0
    if args.poses is None:
        parser.error("features and predict need --poses")
    rows = mesh_rows(args.path, args.poses)
    if args.command == "features":
        for row in rows:
            print(" ".join("%.6g" % value for value in row))
        return 0
    ms = predict(np.loadtxt(args.weights), rows)
    for index, value in enumerate(ms):
        print("pose %d: %.3f ms" % (index, value))
    print("mean: %.3f ms" % ms.mean())
    return 0


if __name__ == "__main__":
    sys.exit(main())
