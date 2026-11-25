"""Checks the probe views (r3d/views/probes.py): placement, the inside test on a closed box, the greedy pick, and
`select` with targets, a caller's cut-out query and its size arguments."""

import hashlib
import pathlib
import platform
import sys
import tempfile
import unittest
from types import SimpleNamespace
from unittest import mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

try:
    import numpy as np

    from r3d.geometry import face_normals
    from r3d.views import probes
except ImportError:
    np = None

from tests.r3d_env import needs_llvm  # noqa: E402

# sha256 of sphere_directions(RAYS) on Linux x86-64; a change to the directions changes it on purpose.
PINNED_DIRECTIONS_SHA256 = "85acf027558ee8ad60a21aa0dd72c81e8bd48dafd475c0b3ea88f198284f109b"


def closed_box(half):
    """A cube of side 2 `half` at the origin, its faces wound outward."""
    corners = np.array([[x, y, z] for x in (-half, half) for y in (-half, half) for z in (-half, half)], dtype=float)
    quads = ((0, 1, 3, 2), (4, 6, 7, 5), (0, 4, 5, 1), (2, 3, 7, 6), (0, 2, 6, 4), (1, 5, 7, 3))
    tris = np.array([tri for a, b, c, d in quads for tri in ((a, b, c), (a, c, d))])
    return corners, tris


@unittest.skipIf(np is None, "the r3d environment is not installed")
class ProbePlacementTests(unittest.TestCase):
    def test_one_probe_per_cell_stays_inside_its_cell(self):
        lo, hi, spacing = np.zeros(3), np.array([4.0, 2.0, 1.0]), 1.0
        points, cell = probes.probe_points(lo, hi, spacing, seed=3)
        self.assertEqual(len(points), 4 * 2 * 1)
        self.assertEqual(cell, 1.0)
        cells = np.floor(points / spacing)
        self.assertEqual(len({tuple(c) for c in cells}), len(points), "two probes share a cell")
        offset = points - (cells + 0.5) * spacing
        self.assertTrue(np.all(np.abs(offset) <= probes.JITTER * spacing))
        np.testing.assert_array_equal(probes.probe_points(lo, hi, spacing, seed=3)[0], points)

    def test_the_spacing_defaults_to_near_planes_and_widens_under_the_count_cap(self):
        near = 0.5
        lo, hi = np.zeros(3), np.full(3, probes.PROBE_SPACING_NEAR_PLANES * near * 2)
        self.assertEqual(probes.spacing_for(lo, hi, None, near), probes.PROBE_SPACING_NEAR_PLANES * near)
        self.assertEqual(probes.spacing_for(lo, hi, 7.0, near), 7.0)
        spacing = probes.spacing_for(lo, hi, 1e-3, near)
        cells = np.prod(np.maximum(np.ceil((hi - lo) / spacing), 1))
        self.assertLessEqual(cells, probes.PROBE_COUNT_MAX)
        self.assertGreater(cells, probes.PROBE_COUNT_MAX / 2, "the spacing widened past what the cap needs")

    def test_the_ray_directions_take_no_simd_dispatched_numpy_function(self):
        # NumPy computes float64 sin, cos and arccos with AVX-512 kernels where the host has them, an ulp off the
        # AVX2 ones: two runners of one vendor then classified different probes and planned different faces.
        def dispatched(*args, **kwargs):
            raise AssertionError("a SIMD-dispatched NumPy function made a probe direction")
        names = ("sin", "cos", "tan", "arcsin", "arccos", "arctan", "arctan2", "exp", "log", "power")
        with mock.patch.multiple(probes.np, **{name: dispatched for name in names}):
            directions = probes.sphere_directions(probes.RAYS)
        self.assertEqual(directions.shape, (probes.RAYS, 3))
        np.testing.assert_allclose(np.linalg.norm(directions, axis=1), 1.0, atol=1e-12)
        # Fibonacci heights step evenly from pole to pole.
        np.testing.assert_allclose(directions[:, 1], 1 - 2 * (np.arange(probes.RAYS) + 0.5) / probes.RAYS)

    @unittest.skipUnless(sys.platform == "linux" and platform.machine() == "x86_64",
                         "the directions are pinned where r3d/isa.py pins: Linux on x86-64")
    def test_the_ray_directions_are_the_same_bits_on_every_pinned_host(self):
        # An AVX-512 host, the same host with NumPy held to AVX2, and an Intel AVX2 host made these bits.
        digest = hashlib.sha256(probes.sphere_directions(probes.RAYS).tobytes()).hexdigest()
        self.assertEqual(digest, PINNED_DIRECTIONS_SHA256)

    def test_greedy_faces_pick_the_largest_gain_first_and_stop_at_the_plateau(self):
        sets = [np.array([0, 1]), np.arange(10), np.array([9, 10, 11]), np.array([12])]
        picked, gains = probes.greedy_faces(sets, np.zeros(13, bool), plateau=0.15)
        self.assertEqual(picked, [1, 2])
        self.assertEqual(gains, [10, 2], "a face's gain counted triangles an earlier pick shows")
        seen = np.zeros(13, bool)
        seen[:10] = True
        self.assertEqual(probes.greedy_faces(sets, seen)[0], [2, 3], "what the path shows still counted")
        self.assertEqual(probes.greedy_faces(sets, np.zeros(13, bool), limit=1)[0], [1])


@needs_llvm
@unittest.skipIf(np is None, "the r3d environment is not installed")
class ProbeInsideTests(unittest.TestCase):
    def setUp(self):
        from r3d import ray_query

        self.variant = ray_query.VARIANT
        ray_query.VARIANT = "llvm_ad_rgb"
        self.addCleanup(setattr, ray_query, "VARIANT", self.variant)

    def test_a_probe_in_a_closed_box_is_dropped_and_one_near_a_wall_moves_off_it(self):
        from r3d.ray_query import RayQuery

        positions, tris = closed_box(1.0)
        query, normals = RayQuery(positions, tris, "llvm_ad_rgb"), face_normals(positions, tris)[0]
        near = 0.1
        points = np.array([[0.0, 0.0, 0.0], [1.0 + 0.05, 0.0, 0.0], [3.0, 0.0, 0.0]])
        moved, state = probes.classify(query, normals, points, cell=1.0, near=near)
        self.assertEqual(state.tolist(), ["inside", "kept", "kept"])
        self.assertGreater(moved[1, 0], points[1, 0] + near, "the probe near the wall did not move off it")
        np.testing.assert_array_equal(moved[2], points[2])

    def test_a_cube_face_shows_the_wall_it_faces(self):
        from r3d.ray_query import RayQuery

        positions, tris = closed_box(1.0)
        query = RayQuery(positions, tris, "llvm_ad_rgb")
        poses = probes.cube_poses(np.array([[3.0, 0.0, 0.0]]))
        sets = probes.shown(query, len(tris), poses, 16, 16, probes.FACE_LENS)
        facing = face_normals(positions, tris)[0] @ np.array([1.0, 0.0, 0.0]) > 0.5
        self.assertEqual(set(sets[1].tolist()), set(np.flatnonzero(facing).tolist()), "the -x face missed the wall")
        self.assertEqual(len(sets[0]), 0, "the +x face, looking away, showed the box")


@needs_llvm
@unittest.skipIf(np is None, "the r3d environment is not installed")
class SelectTests(unittest.TestCase):
    """A target box inside a closed shell whose texture cuts every texel away, one probe beside the shell."""

    REGION = (np.array([2.5, -0.5, -0.5]), np.array([3.5, 0.5, 0.5])) if np is not None else None
    NEAR, SPACING, SEED, SIZE = 0.05, 1.0, 3, 16

    def setUp(self):
        from PIL import Image
        from r3d import ray_query
        from r3d.obj import Texture

        self.addCleanup(setattr, ray_query, "VARIANT", ray_query.VARIANT)
        ray_query.VARIANT = "llvm_ad_rgb"
        box, box_tris = closed_box(0.5)
        shell, shell_tris = closed_box(1.0)
        positions = np.concatenate([box, shell])
        tris = np.concatenate([box_tris, shell_tris + len(box)])
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        path = pathlib.Path(directory.name) / "cut.png"
        Image.fromarray(np.zeros((2, 2, 4), dtype=np.uint8)).save(path)
        self.source = SimpleNamespace(p=positions, tri_v=tris, uv=np.zeros((len(positions), 2)), tri_t=tris,
                                      tri_m=np.repeat([0, 1], [len(box_tris), len(shell_tris)]),
                                      textures=[None, Texture(path)])
        self.targets = self.source.tri_m == 0

    def query(self, cut):
        from r3d.light import AlphaTest
        from r3d.ray_query import RayQuery

        hit_filter = AlphaTest(self.source, 0.5) if cut else None
        return RayQuery(self.source.p, self.source.tri_v, "llvm_ad_rgb", hit_filter=hit_filter)

    def select(self, **kwargs):
        kwargs.setdefault("select_size", self.SIZE)
        return probes.select(self.source.p, self.source.tri_v, self.REGION, self.NEAR, self.SPACING, self.SEED,
                             **kwargs)

    def test_the_shell_hides_the_targets_until_the_query_cuts_its_texels(self):
        hidden = self.select(query=self.query(cut=False), targets=self.targets)
        self.assertEqual(hidden.training, [], "a face that shows no target was kept")
        self.assertEqual(hidden.record["targets"], np.count_nonzero(self.targets))
        cut = self.select(query=self.query(cut=True), targets=self.targets)
        self.assertGreater(len(cut.training), 0, "no face saw the box through the cut shell")
        facing = face_normals(self.source.p, self.source.tri_v)[0] @ np.array([1.0, 0.0, 0.0]) > 0.5
        self.assertEqual(cut.record["train"]["shown_after"], np.count_nonzero(facing & self.targets),
                         "the kept faces should show the box's +x side and count nothing else")

    def test_placed_training_places_every_probe_and_its_kept_faces_are_the_training_views(self):
        query = self.query(cut=True)
        views = self.select(query=query, targets=self.targets)
        from r3d.views.probe_figures import placed_training

        placed = placed_training(self.source.p, self.source.tri_v, self.NEAR, self.SEED, views, query)
        self.assertEqual(len(placed), views.record["train"]["probes"])
        for fate in ("inside", "near"):
            self.assertEqual(sum(probe["fate"] == fate for probe in placed), views.record["train"][fate], fate)
        faces = [np.concatenate([probe["at"], probes.FACES[face]]) for probe in placed for face in probe["faces"]]
        self.assertGreater(len(faces), 0)
        np.testing.assert_allclose(sorted(map(tuple, faces)), sorted(map(tuple, views.training)))

    def test_every_triangle_as_a_target_is_the_same_as_no_targets(self):
        query = self.query(cut=False)
        plain = self.select(query=query)
        everything = self.select(query=query, targets=np.ones(len(self.source.tri_v), bool))
        self.assertNotIn("targets", plain.record)
        self.assertEqual(everything.record.pop("targets"), len(self.source.tri_v))
        self.assertEqual(everything.record, plain.record)
        np.testing.assert_array_equal(everything.training, plain.training)
        np.testing.assert_array_equal(everything.held_out, plain.held_out)
        self.assertGreater(len(plain.training), 0, "the plain shell showed nothing")

    def path_facing_the_shell(self):
        """A path view from the probe's cell straight at the shell: it shows the shell's +x side and nothing else."""
        return self.SIZE, self.SIZE, probes.FACE_LENS, [np.array([3.0, 0.0, 0.0, -1.0, 0.0, 0.0])]

    def test_held_out_faces_come_from_their_own_probes_and_start_from_nothing_shown(self):
        query = self.query(cut=False)
        facing = face_normals(self.source.p, self.source.tri_v)[0] @ np.array([1.0, 0.0, 0.0]) > 0.5
        shell_side = int(np.count_nonzero(facing & ~self.targets))
        views = self.select(query=query, path=self.path_facing_the_shell())
        self.assertEqual(views.record["path_shown"], shell_side)
        self.assertEqual(views.training, [], "training kept a face that shows only what the path shows")
        self.assertEqual(views.record["train"]["shown_after"], shell_side, "what the path showed went uncounted")
        self.assertEqual(len(views.held_out), 1, "the held-out set started from what the path shows")
        self.assertEqual(views.record["held_out"]["shown_after"], shell_side)
        plain = self.select(query=query)
        self.assertEqual(len(plain.training), 1)
        self.assertFalse(np.array_equal(plain.training[0][:3], plain.held_out[0][:3]),
                         "the held-out probe shares the training probe's jitter")

    def test_the_path_counts_only_what_it_shows_of_the_targets(self):
        path = self.path_facing_the_shell()
        query = self.query(cut=False)
        self.assertEqual(self.select(query=query, path=path, targets=self.targets).record["path_shown"], 0)
        cut = self.select(query=self.query(cut=True), path=path, targets=self.targets)
        facing = face_normals(self.source.p, self.source.tri_v)[0] @ np.array([1.0, 0.0, 0.0]) > 0.5
        self.assertEqual(cut.record["path_shown"], np.count_nonzero(facing & self.targets))
        self.assertEqual(cut.training, [], "the path already shows every target the probe can")

    def test_the_held_out_count_stops_at_its_limit(self):
        box, box_tris = closed_box(0.5)
        wall = np.array([[-2.0, -3.0, -3.0], [-2.0, 3.0, -3.0], [-2.0, 3.0, 3.0], [-2.0, -3.0, 3.0]])
        positions = np.concatenate([box, wall])
        tris = np.concatenate([box_tris, np.array([[0, 1, 2], [0, 2, 3]]) + len(box)])
        region = (np.array([1.0, -2.0, -2.0]), np.array([3.0, 2.0, 2.0]))
        counts = [len(probes.select(positions, tris, region, self.NEAR, 2.0, 7, select_size=12,
                                    held_out_faces=limit).held_out) for limit in (probes.HELD_OUT_FACES, 1)]
        self.assertGreater(counts[0], 1, "the fixture has too few held-out candidates to test the limit")
        self.assertEqual(counts[1], 1)

    def test_the_given_query_traces_and_the_sizes_are_arguments(self):
        from r3d import ray_query

        query = self.query(cut=True)
        with mock.patch.object(ray_query, "RayQuery", side_effect=AssertionError("built its own query")), \
             mock.patch.object(probes, "shown", wraps=probes.shown) as traced:
            views = self.select(query=query, select_size=8, face_lens=0.5, held_out_faces=1)
        self.assertTrue(all(call.args[0] is query for call in traced.call_args_list))
        self.assertEqual({call.args[3:] for call in traced.call_args_list}, {(8, 8, 0.5)})
        self.assertLessEqual(len(views.held_out), 1)
        self.assertEqual(views.face_lens, 0.5, "the views do not carry the lens their faces were traced with")


if __name__ == "__main__":
    unittest.main()
