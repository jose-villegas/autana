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
sys.path.insert(0, str(LAUNCHER / "tools"))

try:
    import numpy as np
    from bake import bake
    from r3d.appearance_simplify import start_mesh
    from r3d.fit.copies import joined, placed_copies
    from r3d.import_settings import flight_camera, lfs_pointer_oid, load_scene, source_files
    from r3d.leaks import BEHIND, OPENING, probe_depth, see_through_count
    from r3d.light import drop_masked
    from r3d.mesh_source import load_source
    from r3d.objects.instances import place
    from r3d.poses import path_poses
    from r3d.ray_query import RayQuery
    from r3d.scene_asset import world_placement
    from render.pinhole_rays import FrontMesh, front_hits, pose_rays
    from tests.r3d_env import needs_llvm  # also keeps Windows LLVM loaded until exit
except ImportError:  # pragma: no cover - the tool tests' environment has them
    np = None
    needs_llvm = unittest.skip("the r3d environment is not installed")

# The landscape panel at half resolution, as the host renderer draws a --quarter 1 frame.
RENDER_SIZE = (224, 184)
# The camera path's step, as a baked renderer's camera-path visibility samples it.
EVERY_MS = 500
# Per scene, the most see-through pixels its bakes may show over the path: the measured sum plus PEG_MARGIN of it.
MOST_SEE_THROUGH = {"bistro": 165414, "sponza": 2306}
PEG_MARGIN = 0.01


def placed(points, scene, job):
    """`points` in the scene, under the renderer's object's world transform."""
    placement = world_placement(scene, job.object)
    return place(points, placement.matrix, placement.position)


def kept_source(scene, job):
    """The renderer's source as its import keeps it: (points, triangles) placed in the scene."""
    settings = job.settings
    source = load_source(settings)
    triangles, materials = source.tri_v, source.tri_m
    if settings.alpha_keep is not None:
        triangles, _, materials, _ = drop_masked(source.p, source.uv, source.tri_v, source.tri_t, source.tri_m,
                                                 source.textures, settings.alpha_keep)
    if settings.thin:
        triangles = triangles[np.array([source.names[m] != settings.thin.material for m in materials], dtype=bool)]
    return placed(source.p, scene, job), triangles


def drawn_bake(scene, job):
    """The bundle the scene draws for the renderer: (points, triangles, double-sided flags) placed in the scene."""
    bundle = bake.fetch(bake.locked_bake(scene.path, job.object.name, "levels" if job.renderer.fit else "mesh"))
    rest = start_mesh(bundle)
    mesh = joined(rest, placed_copies(bundle, rest.flat))
    return placed(mesh.points[mesh.vertex_point], scene, job), np.asarray(mesh.tris), np.asarray(mesh.double, bool)


def merged(parts):
    """One (points, triangles, ...) of several, each part's triangles offset past the points before it."""
    points, triangles, rest, offset = [], [], [], 0
    for part in parts:
        points.append(part[0])
        triangles.append(part[1] + offset)
        rest.append(part[2:])
        offset += len(part[0])
    return (np.concatenate(points), np.concatenate(triangles), *(np.concatenate(column) for column in zip(*rest)))


class Tracer:
    """The source and the drawn bake of a scene's baked renderers, traced from one pose at a time."""

    def __init__(self, source, drawn):
        points, triangles = source
        self.source = RayQuery(points, triangles)
        self.drawn = FrontMesh(*drawn)

    def see_through(self, width, height, lens, near, pose):
        """The pose's see-through pixel count."""
        origins, directions = pose_rays(width, height, lens, near, pose)
        source_depth = probe_depth(self.source, origins, directions)
        _, depth = front_hits(self.drawn, pose[:3], origins, directions)
        return see_through_count(source_depth.reshape(height, width), depth.reshape(height, width))


def baked_scenes():
    """(key, scene) for every demo scene with a camera path and a baked renderer."""
    for path in sorted((LAUNCHER / "demo").glob("*/*.scene.toml")):
        scene = load_scene(path)
        if flight_camera(scene) is not None and any(job.bake for job in scene.renderers):
            yield path.name.removesuffix(".scene.toml"), scene


def wall(x0, x1, y0, y1, z):
    """A quad from (x0, y0) to (x1, y1) at depth z, facing +z."""
    points = np.array([[x0, y0, z], [x1, y0, z], [x1, y1, z], [x0, y1, z]], dtype=float)
    return points, np.array([[0, 1, 2], [0, 2, 3]])


def with_hole(half, hole, z):
    """A square wall of half-width `half` at depth z, facing +z, with a square hole of half-width `hole`."""
    return merged([wall(-half, half, hole, half, z), wall(-half, half, -half, -hole, z),
                   wall(-half, -hole, -hole, hole, z), wall(hole, half, -hole, hole, z)])


@unittest.skipIf(np is None, "the r3d environment is not installed")
@needs_llvm
class SeeThroughTests(unittest.TestCase):
    SIZE, LENS, NEAR = (32, 32), 1.0, 0.1
    POSE = np.array([0.0, 0.0, 5.0, 0.0, 0.0, -1.0])  # at z 5, looking down -z at the wall on z 0

    def count(self, source, drawn, double=False):
        drawn = (*drawn, np.full(len(drawn[1]), double))
        return Tracer(source, drawn).see_through(*self.SIZE, self.LENS, self.NEAR, self.POSE)

    def test_a_hole_the_source_has_closed_sees_through(self):
        closed = wall(-10, 10, -10, 10, 0.0)
        self.assertEqual(self.count(closed, closed), 0)
        self.assertGreater(self.count(closed, with_hole(10, 2, 0.0)), 0)

    def test_a_hole_the_source_has_too_is_not_see_through(self):
        holed = with_hole(10, 2, 0.0)
        self.assertEqual(self.count(holed, holed), 0)

    def test_a_surface_moved_back_less_than_behind_is_not_see_through_and_one_moved_further_is(self):
        closed = wall(-10, 10, -10, 10, 0.0)
        # From z 5 the wall on z 0 is 5 away along the centre ray.
        self.assertEqual(self.count(closed, wall(-10, 10, -10, 10, 5.0 - 5.0 * (BEHIND - 0.1))), 0)
        self.assertGreater(self.count(closed, wall(-10, 10, -10, 10, 5.0 - 5.0 * (BEHIND + 0.1))), 0)

    def test_a_gap_narrower_than_the_opening_is_not_a_region(self):
        closed = wall(-10, 10, -10, 10, 0.0)
        pixel = 2 * self.POSE[2] * self.LENS / self.SIZE[0]
        half_gap = (OPENING - 1) * pixel / 2
        gap = merged([wall(-10, -half_gap, -10, 10, 0.0), wall(half_gap, 10, -10, 10, 0.0)])
        self.assertEqual(self.count(closed, gap), 0)

    def test_a_back_face_lets_the_ray_through_unless_double_sided(self):
        points, triangles = wall(-10, 10, -10, 10, 0.0)
        facing_away = (points, triangles[:, ::-1])
        self.assertGreater(self.count((points, triangles), facing_away), 0)
        self.assertEqual(self.count((points, triangles), facing_away, double=True), 0)

    def test_coincident_twins_cover_the_wall_in_either_order(self):
        points, triangles = closed = wall(-10, 10, -10, 10, 0.0)
        twins = np.concatenate([triangles[:, ::-1], triangles])
        reordered = np.roll(twins[::-1], 1, axis=1)
        counts = [self.count(closed, (points, order)) for order in (twins, reordered)]
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
                tracer = Tracer(merged([kept_source(scene, job) for job in baked]),
                                merged([drawn_bake(scene, job) for job in baked]))
                width, height, lens, near, poses = path_poses(flight_camera(scene), RENDER_SIZE, EVERY_MS,
                                                              either_way_up=False)
                pixels = [tracer.see_through(width, height, lens, near, pose) for pose in poses]
                worst = int(np.argmax(pixels))
                most = round(MOST_SEE_THROUGH[key] * (1 + PEG_MARGIN))
                print(f"{key}: {sum(pixels)} see-through pixels over {len(poses)} poses (ceiling {most}); "
                      f"worst pose {worst} at {worst * EVERY_MS} ms with {pixels[worst]}", flush=True)
                self.assertLessEqual(sum(pixels), most, f"{key}: {sum(pixels)} see-through pixels exceeds {most}")


if __name__ == "__main__":
    unittest.main()
