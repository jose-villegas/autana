"""Object contacts agree with a single-tree oracle and retain their geometric boundary rules."""

import pathlib
import sys
import unittest
from types import SimpleNamespace
from unittest.mock import patch

import numpy as np
from scipy.spatial import cKDTree

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from r3d import geometry
from r3d.objects import contact


SCALE = 8
TOLERANCE = contact.seal_tolerance(SCALE)


def reference_contacts(pos, faces, owners, among):
    corners = pos[faces]
    centres = corners.mean(axis=1)
    radii = np.linalg.norm(corners - centres[:, None], axis=2).max(axis=1)
    tree = cKDTree(centres)
    objects = np.unique(owners)

    def border(source, target):
        touched, hits, unshared = set(), set(), False
        for vertex in np.unique(faces[owners == source]):
            point = pos[vertex]
            candidates = np.asarray(tree.query_ball_point(point, radii.max() + TOLERANCE), dtype=int)
            candidates = candidates[owners[candidates] == target]
            nearby = np.linalg.norm(centres[candidates] - point, axis=1) <= radii[candidates] + TOLERANCE
            candidates = candidates[nearby]
            _, distance = geometry.closest_point_on_triangles(np.broadcast_to(point, (len(candidates), 3)),
                                                              *corners[candidates].transpose(1, 0, 2))
            found = candidates[distance <= TOLERANCE]
            if len(found):
                touched.add(vertex)
                hits.update(faces[found].ravel())
                on_corner = np.all(corners[found].astype(np.float32) == point.astype(np.float32), axis=2)
                unshared |= not on_corner.any()
        return touched, hits, unshared

    result = []
    for index, a in enumerate(objects):
        for b in objects[index + 1:]:
            if among is not None and a not in among and b not in among:
                continue
            from_a, hit_b, unshared_a = border(a, b)
            from_b, hit_a, unshared_b = border(b, a)
            if from_a or from_b:
                result.append((a, b, sorted(from_a | hit_a), sorted(from_b | hit_b), unshared_a or unshared_b))
    return result


def random_scene(seed, dtype, snapped):
    rng = np.random.default_rng(seed)
    corners, owners = [], []
    for obj in range(5):
        origin = rng.uniform(0, 2, 3)
        for _ in range(rng.integers(1, 5)):
            centre = origin + rng.uniform(-0.8, 0.8, 3)
            corners.extend(centre + rng.uniform(-0.8, 0.8, (3, 3)) * rng.choice([0.05, 1, 3]))
            owners.append(obj)
    pos = np.asarray(corners, dtype=dtype)
    if snapped:
        pos = np.rint(pos * SCALE) / SCALE
    return pos, np.arange(len(pos)).reshape(-1, 3), np.array(owners)


class ContactTests(unittest.TestCase):
    def test_seeded_scenes_match_single_tree_oracle(self):
        for seed in range(6):
            for dtype in (np.float32, np.float64):
                for snapped in (False, True):
                    pos, faces, owners = random_scene(seed, dtype, snapped)
                    for among in (None, {np.int64(0), np.int64(3)}):
                        expected = reference_contacts(pos, faces, owners, among)
                        for batch in (1, 5, contact.PAIR_BATCH):
                            with self.subTest(seed=seed, dtype=dtype, snapped=snapped, among=among, batch=batch), \
                                    patch.object(contact, "PAIR_BATCH", batch):
                                actual = contact.contact_pairs(pos, faces, owners, SCALE, among)
                                self.assertEqual([(e.a, e.b, e.vertices_a.tolist(), e.vertices_b.tolist(),
                                                   e.unshared) for e in actual], expected)

    def test_both_sides_include_vertices_of_touched_faces(self):
        pos = np.array([[0, 0, 0], [4, 0, 0], [0, 4, 0], [1, 1, 0], [-1, 0, 0], [0, -1, 0]], dtype=float)
        edge, = contact.contact_pairs(pos, np.array([[2, 0, 1], [5, 3, 4]]), np.array([9, 2]), SCALE)
        self.assertEqual((edge.a, edge.b), (2, 9))
        self.assertEqual(edge.vertices_a.tolist(), [3, 4, 5])
        self.assertEqual(edge.vertices_b.tolist(), [0, 1, 2])
        self.assertTrue(edge.unshared)

    def test_exact_tolerance_counts_and_the_next_float_does_not(self):
        pos = np.array([[0, 0, 0], [4, 0, 0], [0, 4, 0], [1, 0, TOLERANCE],
                        [1, 0, np.nextafter(TOLERANCE, np.inf)]])
        self.assertEqual(contact.surface_border(pos, [3, 4], np.array([[0, 1, 2]]), TOLERANCE),
                         ({3}, {0, 1, 2}, True))

    def test_tolerance_halo_spans_disjoint_object_boxes(self):
        gap = TOLERANCE / 2
        pos = np.array([[0, 0, 0], [4, 0, 0], [0, 4, 0], [0, 0, gap], [4, 0, gap], [0, 4, gap]])
        with patch.object(contact, "_overlapping_pairs", wraps=contact._overlapping_pairs) as sweep:
            edge, = contact.contact_pairs(pos, np.arange(6).reshape(2, 3), np.array([0, 1]), SCALE)
        self.assertTrue(edge.unshared)
        np.testing.assert_array_equal(sweep.call_args.args[1], pos.reshape(2, 3, 3).max(axis=1) + TOLERANCE)

    def test_tolerance_reaches_past_a_corner_in_each_band(self):
        gap = TOLERANCE / 2
        pos = np.array([[0, 0, 0], [8, 0, 0], [0, 8, 0], [16, 0, 0], [17, 0, 0], [16, 1, 0],
                        [8 + gap, 0, 0], [17 + gap, 0, 0]], dtype=np.float32)
        faces = np.arange(6).reshape(2, 3)
        for vertex, face in ((6, 0), (7, 1)):
            with self.subTest(vertex=vertex):
                self.assertEqual(contact.surface_border(pos, [vertex], faces, TOLERANCE),
                                 ({vertex}, set(faces[face]), True))

    def test_band_box_includes_edges_on_both_sides_of_two_axes(self):
        mins, maxes = np.array([-4., -8., -1.]), np.array([4., 8., 1.])
        tree = SimpleNamespace(mins=mins, maxes=maxes, query_ball_point=cKDTree([[0, 0, 0]]).query_ball_point)
        search = SimpleNamespace(corners=np.zeros((1, 3, 3)), groups=((tree, np.array([0]), np.array([1.])),))
        points, on_edge = [], []
        for limit, direction in ((mins, -np.inf), (maxes, np.inf)):
            for axis in (0, 1):
                edge = np.nextafter(limit[axis] + np.copysign(1., direction), direction)
                on_edge.append(len(points))
                for coord in (edge, np.nextafter(edge, direction)):
                    point = np.zeros(3)
                    point[axis] = coord
                    points.append(point)
        with patch.object(tree, "query_ball_point", wraps=tree.query_ball_point) as query:
            list(geometry.triangles_near(search, points, 0., contact.PAIR_BATCH))
        query.assert_called_once()
        np.testing.assert_array_equal(query.call_args.args[0], np.asarray(points)[on_edge])

    def test_among_accepts_numpy_ids_in_either_position(self):
        pos = np.array([[0., 0, 0], [1., 0, 0], [0., 1, 0]])
        faces = np.tile(np.arange(3), (3, 1))
        owners = np.array([6, 2, 4])
        edges = contact.contact_pairs(pos, faces, owners, SCALE, among={np.int64(4)})
        self.assertEqual([(edge.a, edge.b) for edge in edges], [(2, 4), (4, 6)])

    def test_duplicate_vertex_ids_share_one_query_and_keep_their_own_ids(self):
        pos = np.array([[0., 0, 0], [2., 0, 0], [0., 2, 0], [1., 0, 0], [1., 0, 0], [8., 8, 8], [8., 8, 8]])
        with patch.object(contact, "closest_point_on_triangles", wraps=geometry.closest_point_on_triangles) as closest:
            border = contact.surface_border(pos, [3, 4, 5, 6], np.array([[0, 1, 2]]), TOLERANCE)
        self.assertEqual(border, ({3, 4}, {0, 1, 2}, True))
        closest.assert_called_once()
        self.assertEqual(len(closest.call_args.args[0]), 1)
        with patch.object(contact, "_surface_border", wraps=contact._surface_border) as surface:
            edge, = contact.contact_pairs(pos, np.array([[3, 4, 5], [6, 5, 4], [0, 1, 2]]),
                                          np.array([0, 0, 1]), SCALE)
        self.assertEqual(edge.vertices_a.tolist(), [3, 4])
        self.assertEqual(edge.vertices_b.tolist(), [0, 1, 2])
        self.assertEqual(len(surface.call_args_list[0].args[1]), 2)

    def test_corner_sharing_uses_float32_resolution(self):
        ulp = np.spacing(np.float32(1))
        pos = np.array([[1., 0, 0], [2., 0, 0], [1., 1, 0], [1 + float(ulp) / 4, 0, 0],
                        [1 + float(ulp), 0, 0]])
        faces = np.array([[0, 1, 2]])
        self.assertEqual(contact.surface_border(pos, [3], faces, TOLERANCE), ({3}, {0, 1, 2}, False))
        self.assertEqual(contact.surface_border(pos, [4], faces, TOLERANCE), ({4}, {0, 1, 2}, True))

    def test_batches_bound_closest_point_work_even_with_one_crowded_vertex(self):
        pos = np.array([[0., 0, 0], [4., 0, 0], [0., 4, 0], [1., 1, 0], [2., 1, 0], [1., 2, 0]])
        for batch, face_count in ((5, 2), (1, 3)):
            faces = np.tile(np.arange(3), (face_count, 1))
            with self.subTest(batch=batch), patch.object(contact, "PAIR_BATCH", batch), \
                    patch.object(contact, "closest_point_on_triangles", wraps=geometry.closest_point_on_triangles) as closest:
                touched, _, _ = contact.surface_border(pos, [3, 4, 5], faces, TOLERANCE)
                sizes = [len(call.args[0]) for call in closest.call_args_list]
                self.assertEqual(touched, {3, 4, 5})
                self.assertEqual(sum(sizes), len(touched) * face_count)
                self.assertLessEqual(max(sizes), max(batch, face_count))

    def test_empty_queries_and_single_object_have_no_contacts(self):
        pos, faces, owners = random_scene(0, np.float64, False)
        self.assertEqual(contact.surface_border(pos, [], faces, TOLERANCE), (set(), set(), False))
        self.assertEqual(contact.surface_border(pos, [0], faces[:0], TOLERANCE), (set(), set(), False))
        self.assertEqual(contact.contact_pairs(pos[:0], faces[:0], owners[:0], SCALE), [])
        self.assertEqual(contact.contact_pairs(pos, faces[:0], owners[:0], SCALE), [])
        self.assertEqual(contact.contact_pairs(pos, faces, np.zeros_like(owners), SCALE), [])
