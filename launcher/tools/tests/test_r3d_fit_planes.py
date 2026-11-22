import unittest
from types import SimpleNamespace
from unittest import mock

import numpy as np

from r3d.fit.planes import MAX_PLANES, PLANE_MERGE_DEG, PLANE_REACH, plane_loss, start_planes
from r3d import geometry
from r3d.geometry import closest_point_on_triangles, triangle_search, triangles_within


def box():
    """A unit cube, outward faces, two triangles each."""
    p = np.array([[x, y, z] for x in (0, 1) for y in (0, 1) for z in (0, 1)], float)
    quads = [(0, 1, 3, 2), (4, 6, 7, 5), (0, 4, 5, 1), (2, 3, 7, 6), (0, 2, 6, 4), (1, 5, 7, 3)]
    tris = [t for a, b, c, d in quads for t in ((a, b, c), (a, c, d))]
    return SimpleNamespace(p=p, tri_v=np.array(tris))


def grid(bend_deg=0.0, n=4):
    """A flat n x n grid in z = 0 whose right half turns up about x = n/2 by `bend_deg`."""
    xs = np.arange(n + 1, dtype=float)
    p = np.array([[x, y, 0.0] for y in xs for x in xs])
    half = n / 2
    turn = np.radians(bend_deg)
    right = p[:, 0] > half
    p[right, 2] = (p[right, 0] - half) * np.sin(turn)
    p[right, 0] = half + (p[right, 0] - half) * np.cos(turn)
    tris = []
    for y in range(n):
        for x in range(n):
            a = y * (n + 1) + x
            tris += [(a, a + 1, a + n + 2), (a, a + n + 2, a + n + 1)]
    return SimpleNamespace(p=p, tri_v=np.array(tris))


def planes_at(source, point, scale=1.0):
    rows, normals = start_planes(np.array([point], float), np.array([scale]), source)
    return normals[rows == 0]


class TrianglesWithinTests(unittest.TestCase):
    def test_every_triangle_within_reach_is_found_and_none_beyond(self):
        rng = np.random.default_rng(3)
        corners = rng.uniform(0, 10, (300, 3, 3)) * np.array([1, 1, 0.1]) + rng.uniform(0, 1, (300, 1, 3))
        corners[:5] *= 4  # a few large triangles in their own size group
        points = rng.uniform(0, 10, (40, 3))
        reach = rng.uniform(0.2, 1.5, 40)
        search = triangle_search(corners)
        query, found = triangles_within(points, search, reach)
        # Chunks smaller than the query keep every point's own reach and index.
        with mock.patch.object(geometry, "QUERY_CHUNK", 7):
            chunked = triangles_within(points, search, reach)
        self.assertEqual(set(zip(*(part.tolist() for part in chunked))), set(zip(query.tolist(), found.tolist())))
        _here, distance = closest_point_on_triangles(np.repeat(points, len(corners), 0),
                                                     *(np.tile(corners[:, k], (len(points), 1)) for k in range(3)))
        distance = distance.reshape(len(points), len(corners))
        expected = {(int(q), int(t)) for q, t in zip(*np.nonzero(distance <= reach[:, None]))}
        self.assertTrue(expected)
        self.assertEqual(set(zip(query.tolist(), found.tolist())), expected)


class StartPlaneTests(unittest.TestCase):
    def test_a_point_inside_a_face_has_one_plane_on_an_edge_two_at_a_corner_three(self):
        cube = box()
        self.assertEqual(len(planes_at(cube, (0.5, 0.5, 0.0))), 1)
        self.assertEqual(len(planes_at(cube, (0.5, 0.0, 0.0))), 2)
        self.assertEqual(len(planes_at(cube, (0.0, 0.0, 0.0))), MAX_PLANES)

    def test_coplanar_faces_around_a_point_are_one_plane(self):
        normals = planes_at(grid(), (2.0, 2.0, 0.0))
        self.assertEqual(len(normals), 1)
        self.assertAlmostEqual(abs(normals[0][2]), 1.0)

    def test_a_fold_under_the_merge_angle_is_one_plane_and_over_it_two(self):
        self.assertEqual(len(planes_at(grid(PLANE_MERGE_DEG / 2), (2.0, 2.0, 0.0))), 1)
        self.assertEqual(len(planes_at(grid(PLANE_MERGE_DEG * 2), (2.0, 2.0, 0.0))), 2)

    def test_a_double_sided_wall_is_one_plane(self):
        flat = grid()
        both = SimpleNamespace(p=flat.p, tri_v=np.concatenate([flat.tri_v, flat.tri_v[:, ::-1]]))
        self.assertEqual(len(planes_at(both, (1.0, 1.0, 0.0))), 1)

    def test_a_point_off_every_face_takes_the_nearest_ones(self):
        self.assertEqual(len(planes_at(grid(), (2.0, 2.0, 0.3))), 1)

    def test_the_reach_scales_with_the_point_so_a_face_past_it_is_not_taken(self):
        gap = 2 * PLANE_REACH  # from a point inside the cube's bottom face to its y = 1 face
        point = (0.5, 1.0 - gap, 0.0)
        self.assertEqual(len(planes_at(box(), point, scale=1.0)), 1)
        self.assertEqual(len(planes_at(box(), point, scale=4.0)), 2)


class PlaneLossTests(unittest.TestCase):
    def setUp(self):
        import torch

        self.torch = torch
        self.start = np.array([[0.5, 0.5, 0.0], [0.5, 0.0, 0.0], [0.0, 0.0, 0.0]])
        self.scale = np.ones(3)
        self.rows, normals = start_planes(self.start, self.scale, box())
        self.args = (torch.as_tensor(self.rows), torch.as_tensor(normals, dtype=torch.float32))

    def loss(self, moved):
        t = self.torch
        return float(plane_loss(t.as_tensor(moved, dtype=t.float32), t.as_tensor(self.start, dtype=t.float32),
                                *self.args, t.as_tensor(self.scale, dtype=t.float32)))

    def test_a_point_sliding_in_its_face_or_along_its_edge_costs_nothing(self):
        moved = self.start + np.array([[0.3, -0.2, 0.0], [0.4, 0.0, 0.0], [0.0, 0.0, 0.0]])
        self.assertAlmostEqual(self.loss(moved), 0.0, places=6)

    def test_leaving_a_plane_costs_its_squared_distance_in_local_scales(self):
        for point, step in ((0, (0, 0, 0.1)), (1, (0, 0.1, 0)), (2, (0.1, 0, 0))):
            moved = self.start.copy()
            moved[point] += step
            self.assertAlmostEqual(self.loss(moved), 0.01 / len(self.rows), places=6)

    def test_no_rows_cost_nothing(self):
        t = self.torch
        empty = (t.zeros(0, dtype=t.long), t.zeros((0, 3)))
        start = t.as_tensor(self.start, dtype=t.float32)
        self.assertEqual(float(plane_loss(start + 1, start, *empty, t.ones(3))), 0.0)


if __name__ == "__main__":
    unittest.main()
