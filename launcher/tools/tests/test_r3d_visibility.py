"""Checks the camera-path visibility rule on small scenes: coincident faces,
the view's frustum and margin, the import step that calls it, and that what
it keeps covers poses between the sampled ones. Needs the pinned r3d
environment (tools/r3d/requirements.txt)."""

import pathlib
import sys
import unittest
from types import SimpleNamespace
from unittest import mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

try:
    import numpy as np
    from tests import soup

    from r3d import mesh_import
    from r3d.light import coincident_faces, visible_from_path
    from r3d.poses import camera_rays
except ImportError:
    np = None

EYE = [0.0, 0.0, 5.0, 0.0, 0.0, -1.0]


def card(centre, half=1.0, away=False):
    """A square card facing +z (or -z) as four corners and two triangles."""
    x, y, z = centre
    corners = [[x - half, y - half, z], [x + half, y - half, z], [x + half, y + half, z], [x - half, y + half, z]]
    faces = [[0, 2, 1], [0, 3, 2]] if away else [[0, 1, 2], [0, 2, 3]]
    return corners, faces


def scene(*cards):
    positions, tris = [], []
    for corners, faces in cards:
        tris += [[len(positions) + i for i in face] for face in faces]
        positions += corners
    positions, tris = np.array(positions, dtype=float), np.array(tris)
    return positions, tris, soup.rays(soup.Soup(positions, tris))


def visible(positions, tris, intersector, poses=(EYE,), size=(16, 12), lens=0.62, near=0.5, samples=2, margin=0, **options):
    poses = [np.array(pose, dtype=float) for pose in poses]
    return visible_from_path(positions, tris, np.zeros(len(tris), dtype=bool), intersector, poses, *size, lens, near, samples,
                             margin, **options)


@unittest.skipIf(np is None, "the r3d environment is not installed")
class CoincidentFaceTests(unittest.TestCase):
    def test_twins_need_the_other_winding_and_duplicates_share_a_group(self):
        positions, tris, _ = scene(card([0, 0, 0]), card([0, 0, 0], away=True), card([0, 0, 0]))
        twin, group = coincident_faces(positions, tris)
        self.assertEqual(twin[[0, 1, 4, 5]].tolist(), [2, 3, 2, 3])
        self.assertEqual(twin[[2, 3]].tolist(), [0, 1])
        self.assertEqual(group.tolist(), [0, 1, 2, 3, 0, 1])

    def test_a_same_winding_duplicate_seen_from_behind_is_dropped(self):
        positions, tris, intersector = scene(card([0, 0, 0], away=True), card([0, 0, 0], away=True), card([0, 0, -1]))
        self.assertEqual(visible(positions, tris, intersector).tolist(), [False] * 4 + [True] * 2)

    def test_a_coincident_triple_keeps_both_front_faces_and_drops_the_back(self):
        positions, tris, intersector = scene(card([0, 0, 0]), card([0, 0, 0], away=True), card([0, 0, 0]), card([0, 0, -1]))
        self.assertEqual(visible(positions, tris, intersector).tolist(), [True, True, False, False, True, True, False, False])

    def test_faces_within_the_tie_are_kept_and_beyond_it_hidden(self):
        positions, tris, intersector = scene(card([0, 0, 0]), card([0, 0, -0.05]))
        self.assertEqual(visible(positions, tris, intersector, tie=0.1).tolist(), [True] * 4)
        self.assertEqual(visible(positions, tris, intersector, tie=0.01).tolist(), [True, True, False, False])


@unittest.skipIf(np is None, "the r3d environment is not installed")
class FrustumTests(unittest.TestCase):
    def test_a_card_beside_the_view_is_dropped_and_one_inside_kept(self):
        positions, tris, intersector = scene(card([0, 0, 0], half=0.5), card([9.0, 0, 0], half=0.5))
        self.assertEqual(visible(positions, tris, intersector).tolist(), [True, True, False, False])

    def test_the_margin_reaches_a_card_just_outside_the_edge(self):
        # The view's right edge at depth 5 is x = 5 * lens * width / height; a card a little past it.
        width, height, lens = 16, 12, 0.62
        edge = 5.0 * lens * width / height
        pixel = 2 * edge / width
        positions, tris, intersector = scene(card([edge + 0.5 * pixel + 0.3 * pixel, 0, 0], half=0.3 * pixel))
        self.assertFalse(visible(positions, tris, intersector, samples=3, margin=0).any())
        self.assertTrue(visible(positions, tris, intersector, samples=3, margin=1).all())

    def test_the_margin_widens_the_rays_by_exactly_that_many_pixels(self):
        width, height, lens, samples = 16, 12, 0.62, 2
        for margin in (0, 3):
            origin, direction = camera_rays(width, height, lens, [0, 0, 0], [0, 0, -1], samples, margin)
            self.assertEqual(len(direction), (width + 2 * margin) * (height + 2 * margin) * samples * samples)
            x = direction[:, 0] / -direction[:, 2]
            step = 2 * lens * width / height / width
            self.assertAlmostEqual(x.max(), lens * width / height + (margin - 0.25) * step, places=9)


@unittest.skipIf(np is None, "the r3d environment is not installed")
class ImportWiringTests(unittest.TestCase):
    def test_the_import_step_calls_the_source_its_settings_name(self):
        camera = SimpleNamespace(half_fov_short_tan=0.62, near_z=6.0, path=SimpleNamespace(clip="fly", node="camera"))
        scene_ = SimpleNamespace(camera=SimpleNamespace(component=camera), region=([0, 0, 0], [1, 1, 1]), path=pathlib.Path("out") / "hall.scene.toml")
        path = SimpleNamespace(source="camera_path", every_ms=100, size=(184, 224), samples=3, margin=8)
        region = SimpleNamespace(source="camera_region", rounds=4)
        poses = (184, 224, 0.62, 6.0, ["pose"])
        with mock.patch.object(mesh_import, "sample_camera_path", return_value=poses) as sampled, \
                mock.patch.object(mesh_import, "visible_from_path", return_value="path") as by_path, \
                mock.patch.object(mesh_import, "visible_from_region", return_value="region") as by_region:
            self.assertEqual(mesh_import.visible_triangles(path, scene_, "p", "t", "d", "i", "rng"), "path")
            self.assertEqual(mesh_import.visible_triangles(region, scene_, "p", "t", "d", "i", "rng"), "region")
        sampled.assert_called_once_with(pathlib.Path("out") / "fly_tracks_generated.c", "fly", "camera", 100, 184, 224, 0.62, 6.0)
        # A square view as wide as the long side covers the panel either way up.
        by_path.assert_called_once_with("p", "t", "d", "i", ["pose"], 224, 224, 0.62 * 224 / 184, 6.0, 3, 8)
        by_region.assert_called_once_with("p", "t", "d", "i", 4, "rng", [0, 0, 0], [1, 1, 1])


@unittest.skipIf(np is None, "the r3d environment is not installed")
class BetweenPoseTests(unittest.TestCase):
    def test_what_is_kept_covers_the_poses_between_the_samples(self):
        # A corridor of pillars walked down its middle: every face a ray from a pose halfway between two samples
        # draws first must be among those the samples kept.
        cards = [card([x, 0, -z], half=0.4) for z in (2, 4, 6, 8) for x in (-1.5, 0.0, 1.5)]
        cards += [card([0, 0, -12], half=3.0)]
        positions, tris, intersector = scene(*cards)
        samples = [[x, 0.0, 6.0, 0.15 * x, 0.0, -1.0] for x in np.linspace(-1.0, 1.0, 9)]
        kept = visible(positions, tris, intersector, poses=samples, size=(24, 18), samples=3, margin=2)
        self.assertFalse(kept.all(), "the corridor hides nothing: the test proves nothing")
        normal = np.cross(positions[tris[:, 1]] - positions[tris[:, 0]], positions[tris[:, 2]] - positions[tris[:, 0]])
        for a, b in zip(samples, samples[1:]):
            pose = (np.array(a) + np.array(b)) / 2
            origin, direction = camera_rays(24, 18, 0.62, pose[:3], pose[3:], 1, 0)
            found, _, tri = intersector.first_hits(origin, direction)
            hit = np.where(found, tri, -1)
            drawn = hit[(hit >= 0)]
            drawn = drawn[(normal[drawn] * direction[hit >= 0]).sum(axis=1) < 0]
            self.assertTrue(kept[drawn].all(), f"a face drawn from {pose[:3]} was culled")


if __name__ == "__main__":
    unittest.main()
