"""Bound exact point-to-triangle searches with a tree of triangle boxes."""
import ctypes
import functools
from pathlib import Path

import numpy as np
from scipy.spatial import cKDTree

from native.shared_library import build_shared, find_compiler
from r3d.geometry import SEEDS, closest_point_on_triangles, nearest_triangles, triangle_search

HERE = Path(__file__).resolve().parent
SOURCE = HERE / "triangle_bounds.c"
CANDIDATE_CAPACITY = 1 << 18
# Roundoff in a triangle's weighted point and the squared box distance.
BOUNDS_EPSILON = 8 * np.finfo(np.float64).eps


class BoundsIndex(ctypes.Structure):
    _fields_ = [(name, ctypes.c_void_p)
                for name in ("low", "high", "children", "start", "count", "order", "triangle_low", "triangle_high")]


@functools.cache
def candidates_function():
    library = build_shared("triangle_bounds", HERE / ".cache", (SOURCE,),
                           find_compiler("CC", ("cc", "gcc", "clang"), "C"), flags=("-ffp-contract=off",))
    function = library.triangle_bound_candidates
    function.argtypes = [ctypes.POINTER(BoundsIndex), ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
                         ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t]
    function.restype = ctypes.c_size_t
    return function


class TriangleBounds:
    """The centroid tree's topology with bounds enclosing the triangles in each node."""

    def __init__(self, corners):
        self.corners = np.ascontiguousarray(corners, dtype=np.float64)
        self.tree = cKDTree(self.corners.mean(axis=1))
        order = np.asarray(self.tree.indices, dtype=np.int64)
        triangle_low, triangle_high = self.corners.min(axis=1), self.corners.max(axis=1)
        nodes = []

        def pack(node):
            row = len(nodes)
            nodes.append(None)
            if node.split_dim < 0:
                start, stop = node.start_idx, node.end_idx
                indices = order[start:stop]
                low, high = triangle_low[indices].min(axis=0), triangle_high[indices].max(axis=0)
                nodes[row] = (low, high, -1, -1, start, stop - start)
            else:
                left, right = pack(node.lesser), pack(node.greater)
                low = np.minimum(nodes[left][0], nodes[right][0])
                high = np.maximum(nodes[left][1], nodes[right][1])
                nodes[row] = (low, high, left, right, 0, 0)
            return row

        pack(self.tree.tree)
        low, high, left, right, start, count = zip(*nodes)
        self.arrays = (np.array(low), np.array(high), np.column_stack((left, right)).astype(np.int64),
                       np.array(start, dtype=np.int64), np.array(count, dtype=np.int64), order,
                       triangle_low, triangle_high)
        self.index = BoundsIndex(*(column.ctypes.data for column in self.arrays))
        self.scale = max(np.abs(low[0]).max(), np.abs(high[0]).max())
        self.fallback = None

    def distances(self, points):
        """Exact distances, using box tests to reject triangles outside the seeded upper bound."""
        points = np.ascontiguousarray(points, dtype=np.float64).reshape(-1, 3)
        best = np.full(len(points), np.inf)
        if not len(points):
            return best

        def consider(owners, triangles):
            faces = self.corners[triangles]
            _, distances = closest_point_on_triangles(points[owners], *faces.transpose(1, 0, 2))
            np.fmin.at(best, owners, distances)

        seeds = min(SEEDS, len(self.corners))
        neighbours = self.tree.query(points, k=seeds)[1].reshape(-1)
        consider(np.repeat(np.arange(len(points)), seeds), neighbours)
        rounding = BOUNDS_EPSILON * np.maximum(self.scale, np.abs(points).max(axis=1))
        reach_squared = (best + rounding) ** 2
        owners, triangles = np.empty((2, CANDIDATE_CAPACITY), dtype=np.int64)
        count = candidates_function()(ctypes.byref(self.index), points.ctypes.data, reach_squared.ctypes.data,
                                      len(points), owners.ctypes.data, triangles.ctypes.data, len(owners))
        if count > len(owners):
            if self.fallback is None:
                self.fallback = triangle_search(self.corners)
            return nearest_triangles(points, self.fallback)[2]
        consider(owners[:count], triangles[:count])
        return best
