"""A video scored against traced reference frames must stand at their poses."""
import pathlib
import json

import numpy as np
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "render"))
import reference_poses  # noqa: E402

TRACED = "size 8 8\nlens 0.6 1\npose 1 2 3 0 0 1\npose 4 5 6 1 0 0\n"
HEADER = "frame,ms,px,py,pz,qx,qy,qz,qw\n"
# The engine's z runs opposite the source's, so the same two poses in its axes:
# (1, 2, -3) turned half a turn about y looks along the source's +z, and
# (4, 5, -6) turned a quarter turn about y looks along +x.
FIRST = "0,0,1,2,-3,0,1,0,0\n"
SECOND = "1,100,4,5,-6,0,0.70710678,0,0.70710678\n"
DRAWN = HEADER + FIRST + SECOND
STILL = HEADER + "0,0,0,0,0,0,0,0,1\n1,100,0,0,0,0,0,0,1\n"


class ReferencePoses(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.temp.name)
        self.reference = self.root / "reference"
        self.reference.mkdir()
        self.video = self.root / "render.avi"

    def tearDown(self):
        self.temp.cleanup()

    def traced(self, text=TRACED):
        (self.reference / reference_poses.REFERENCE_POSES).write_text(text)

    def drawn(self, rows):
        pathlib.Path(str(self.video) + reference_poses.VIDEO_POSES_SUFFIX).write_text(rows)

    def check(self, frames, first=0):
        """Check video frames `first` .. `frames` - 1 as reference_video walks them."""
        stands = reference_poses.checker(self.video, self.reference)
        for frame in range(first, frames):
            stands(frame, frame - first)

    def test_capture_views_preserve_camera_roll(self):
        directory = self.root / 'capture'
        directory.mkdir()
        (directory / 'entries.json').write_text(json.dumps(dict(width=4, height=2, lens=0.5, near=1.0)))
        (directory / 'capture.poses.csv').write_text(
            HEADER + '0,0,0,0,0,0,0,0.70710678,0.70710678\n')
        [(matrix, size)] = reference_poses.capture_views(directory / 'capture.poses.csv')
        self.assertEqual(size, (4, 2))
        np.testing.assert_allclose(matrix[0, :3], [0, 1, 0], atol=reference_poses.POSE_TOLERANCE)
        np.testing.assert_allclose(matrix[1, :3], [-2, 0, 0], atol=reference_poses.POSE_TOLERANCE)

    def test_capture_projection_uses_display_aspect(self):
        path = self.root / 'capture.poses.csv'
        path.write_text(STILL)
        (self.root / 'entries.json').write_text(json.dumps(dict(
            width=8, height=8, viewport_width=12, viewport_height=8, lens=0.5, near=1.0)))
        matrix, size = reference_poses.capture_views(path)[0]
        self.assertEqual(size, (8, 8))
        np.testing.assert_allclose(np.diag(matrix)[:2], [4 / 3, 2])

    def test_capture_poses_requires_the_suffix_and_no_frame_gaps_and_takes_size_override(self):
        path = self.root / 'capture.poses.csv'
        (self.root / 'entries.json').write_text(json.dumps(dict(width=4, height=2, lens=0.5, near=1.0)))
        path.write_text(DRAWN)
        with self.assertRaisesRegex(ValueError, 'expected a .*pose log'):
            reference_poses.capture_poses(self.root / 'capture.csv')
        for override, expected in ((None, (4, 2)), ((7, 3), (7, 3))):
            width, height, lens, near, poses = reference_poses.capture_poses(path, override)
            self.assertEqual((width, height), expected)
            self.assertEqual((lens, near, len(poses)), (0.5, 1.0, 2))
        path.write_text(HEADER + FIRST + SECOND.replace('1,100,', '2,100,', 1))
        with self.assertRaisesRegex(ValueError, 'poses must cover every frame'):
            reference_poses.capture_poses(path)

    def test_a_video_at_its_references_poses_passes(self):
        self.traced()
        self.drawn(DRAWN)
        self.check(2)

    def test_a_still_camera_is_refused_naming_the_frame(self):
        self.traced()
        self.drawn(STILL)
        with self.assertRaisesRegex(ValueError, "frame 0 stands at .* not at the same pose"):
            self.check(2)

    def test_a_turned_camera_at_the_right_place_is_refused(self):
        self.traced()
        self.drawn(HEADER + "0,0,1,2,-3,0,0,0,1\n")
        with self.assertRaisesRegex(ValueError, "frame 0"):
            self.check(1)

    def test_skipped_video_frames_line_up_with_the_first_reference_and_name_the_video_frame(self):
        self.traced("size 8 8\nlens 0.6 1\npose 4 5 6 1 0 0\npose 9 9 9 1 0 0\n")
        self.drawn(DRAWN + "2,200,4,5,-6,0,0.70710678,0,0.70710678\n")
        with self.assertRaisesRegex(ValueError, r"render\.avi frame 2 stands .* its reference frame 1 at"):
            self.check(3, first=1)

    def test_rows_are_matched_by_their_frame_not_their_order(self):
        self.traced()
        self.drawn(HEADER + SECOND + FIRST)
        self.check(2)

    def test_a_scored_frame_without_a_row_is_refused(self):
        # The harness writes no row for a frame with no active camera; an empty
        # or truncated log must not pass for the frames it lacks.
        self.traced()
        for rows in (HEADER, HEADER + FIRST, HEADER + SECOND):
            with self.subTest(rows=rows), self.assertRaisesRegex(ValueError, "has no row .* wrote no camera pose"):
                self.drawn(rows)
                self.check(2)

    def test_a_frame_past_the_traced_poses_is_refused(self):
        self.traced("size 8 8\nlens 0.6 1\npose 1 2 3 0 0 1\n")
        self.drawn(DRAWN)
        with self.assertRaisesRegex(ValueError, "frame 1 has no pose in .* traces 1"):
            self.check(2)

    def test_the_tolerance_is_a_hundredth_either_side(self):
        self.traced()
        inside = reference_poses.POSE_TOLERANCE * 0.9
        outside = reference_poses.POSE_TOLERANCE * 1.1
        self.drawn(HEADER + f"0,0,{1 + inside},2,-3,0,1,0,0\n")
        self.check(1)
        self.drawn(HEADER + f"0,0,{1 + outside},2,-3,0,1,0,0\n")
        with self.assertRaisesRegex(ValueError, "frame 0"):
            self.check(1)

    def test_a_video_without_poses_is_refused_by_a_reference_that_has_them(self):
        self.traced()
        with self.assertRaisesRegex(ValueError, "no .poses.csv beside it"):
            reference_poses.checker(self.video, self.reference)

    def test_a_reference_without_poses_is_not_checked(self):
        self.assertIsNone(reference_poses.checker(self.video, self.reference))


if __name__ == "__main__":
    unittest.main()
