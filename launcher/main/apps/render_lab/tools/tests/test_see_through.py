"""See-through: where a scene's source closes the view but its drawn bake lets the ray past.

    python -m unittest discover -s launcher/main/apps/render_lab/tools/tests -p test_see_through.py

Every demo scene with a camera path is sampled every EVERY_MS at RENDER_SIZE, and each pixel's centre ray is traced into
the source as the import keeps it (alpha-masked triangles and the whole thinned material left out, where the import
keeps a seeded share of it) and into the bundles the scene draws (the rest at its finest level and its placed copies).
Each pose traces the first drawn hit after culling back faces from the camera eye. Masked card copies are outside the
trace. Only baked renderers are traced.

A pixel sees through where the source hits and the bake hits nothing or lies beyond BEHIND times the source's distance.
Opening by OPENING removes silhouettes a pixel wide. The remaining pixels across the path must fit the scene's ceiling
from MOST_SEE_THROUGH and PEG_MARGIN.

Needs the r3d environment (launcher/tools/r3d/requirements.txt), the locked bakes (bake.py fetch) and the source meshes
(Git LFS).
"""
import pathlib
import sys
import unittest

TOOLS = pathlib.Path(__file__).resolve().parents[1]
LAUNCHER = TOOLS.parents[3]
sys.path.insert(0, str(TOOLS))
sys.path.insert(0, str(LAUNCHER / "tools"))

try:
    import numpy as np
    from r3d.import_settings import lfs_pointer_oid, source_files
    from see_through import (BEHIND, EVERY_MS, MOST_SEE_THROUGH, PEG_MARGIN, Tracer, baked_scenes, measure_see_through,
                             merged, see_through_ceiling)
    from tests.r3d_env import needs_llvm  # also keeps Windows LLVM loaded until exit
except ImportError:  # pragma: no cover - the tool tests' environment has them
    np = None
    needs_llvm = unittest.skip("the r3d environment is not installed")

def wall(x0, x1, y0, y1, z):
    """A quad from (x0, y0) to (x1, y1) at depth z, facing +z."""
    points = np.array([[x0, y0, z], [x1, y0, z], [x1, y1, z], [x0, y1, z]], dtype=float)
    return points, np.array([[0, 1, 2], [0, 2, 3]])


def with_hole(half, hole, z):
    """A square wall of half-width `half` at depth z, facing +z, with a square hole of half-width `hole`."""
    return merged([wall(-half, half, hole, half, z), wall(-half, half, -half, -hole, z),
                   wall(-half, -hole, -hole, hole, z), wall(hole, half, -hole, hole, z)])


@unittest.skipIf(np is None, "the r3d environment is not installed")
class SeeThroughCeiling(unittest.TestCase):
    def test_ceiling_is_the_measured_total_plus_the_rounded_peg_margin(self):
        for key, measured in MOST_SEE_THROUGH.items():
            with self.subTest(scene=key):
                self.assertEqual(see_through_ceiling(key), round(measured * (1 + PEG_MARGIN)))
        self.assertIsNone(see_through_ceiling("unknown"))


@unittest.skipIf(np is None, "the r3d environment is not installed")
@needs_llvm
class SeeThroughTests(unittest.TestCase):
    SIZE, LENS, NEAR = (32, 32), 1.0, 0.1
    POSE = np.array([0.0, 0.0, 5.0, 0.0, 0.0, -1.0])  # at z 5, looking down -z at the wall on z 0

    def mask(self, source, drawn, double=False):
        drawn = (*drawn, np.full(len(drawn[1]), double))
        return Tracer(source, drawn).see_through(*self.SIZE, self.LENS, self.NEAR, self.POSE)

    def test_a_hole_the_source_has_closed_sees_through(self):
        closed = wall(-10, 10, -10, 10, 0.0)
        self.assertEqual(self.mask(closed, closed).sum(), 0)
        self.assertGreater(self.mask(closed, with_hole(10, 2, 0.0)).sum(), 0)

    def test_a_hole_the_source_has_too_is_not_see_through(self):
        holed = with_hole(10, 2, 0.0)
        self.assertEqual(self.mask(holed, holed).sum(), 0)

    def test_a_surface_moved_back_less_than_behind_is_not_see_through_and_one_moved_further_is(self):
        closed = wall(-10, 10, -10, 10, 0.0)
        # From z 5 the wall on z 0 is 5 away along the centre ray.
        self.assertEqual(self.mask(closed, wall(-10, 10, -10, 10, 5.0 - 5.0 * (BEHIND - 0.1))).sum(), 0)
        self.assertGreater(self.mask(closed, wall(-10, 10, -10, 10, 5.0 - 5.0 * (BEHIND + 0.1))).sum(), 0)

    def test_a_gap_narrower_than_the_opening_is_not_a_region(self):
        closed = wall(-10, 10, -10, 10, 0.0)
        # A pixel at the wall spans 2 * 5 * LENS / 32 = 0.3125 across: a gap 0.2 wide misses at most one column.
        gap = merged([wall(-10, -0.1, -10, 10, 0.0), wall(0.1, 10, -10, 10, 0.0)])
        self.assertEqual(self.mask(closed, gap).sum(), 0)

    def test_a_back_face_lets_the_ray_through_unless_double_sided(self):
        points, triangles = wall(-10, 10, -10, 10, 0.0)
        facing_away = (points, triangles[:, ::-1])
        self.assertGreater(self.mask((points, triangles), facing_away).sum(), 0)
        self.assertEqual(self.mask((points, triangles), facing_away, double=True).sum(), 0)

    def test_coincident_twins_cover_the_wall_in_either_order(self):
        points, triangles = closed = wall(-10, 10, -10, 10, 0.0)
        twins = np.concatenate([triangles[:, ::-1], triangles])
        reordered = np.roll(twins[::-1], 1, axis=1)
        counts = [int(self.mask(closed, (points, order)).sum()) for order in (twins, reordered)]
        self.assertEqual(counts, [0, 0])


@unittest.skipIf(np is None, "the r3d environment is not installed")
@needs_llvm
class SeeThroughBakes(unittest.TestCase):
    def test_every_scene_with_a_path_lists_its_ceiling(self):
        self.assertEqual(sorted(key for key, _ in baked_scenes()), sorted(MOST_SEE_THROUGH))

    def test_each_bake_sees_through_no_more_than_its_ceiling(self):
        for key, scene in baked_scenes():
            with self.subTest(scene=key):
                baked = [job for job in scene.renderers if job.bake]
                unpulled = [path for job in baked for path in source_files(job.settings) if lfs_pointer_oid(path)]
                if unpulled:
                    self.skipTest(f"{key}: {unpulled[0]} is a Git LFS pointer; run git lfs pull --exclude=\"\"")
                pixels = measure_see_through(scene)
                worst = int(np.argmax(pixels))
                most = see_through_ceiling(key)
                print(f"{key}: {sum(pixels)} see-through pixels over {len(pixels)} poses (ceiling {most}); "
                      f"worst pose {worst} at {worst * EVERY_MS} ms with {pixels[worst]}", flush=True)
                self.assertLessEqual(sum(pixels), most, f"{key}: {sum(pixels)} see-through pixels exceeds {most}")


if __name__ == "__main__":
    unittest.main()
