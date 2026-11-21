"""A video scored against traced reference frames must stand at their poses."""
import pathlib
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
DRAWN = HEADER + "0,0,1,2,-3,0,1,0,0\n1,100,4,5,-6,0,0.70710678,0,0.70710678\n"
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

    def drawn(self, rows):
        pathlib.Path(str(self.video) + reference_poses.VIDEO_POSES_SUFFIX).write_text(rows)

    def test_a_video_at_its_references_poses_passes(self):
        (self.reference / reference_poses.REFERENCE_POSES).write_text(TRACED)
        self.drawn(DRAWN)
        reference_poses.check(self.video, self.reference)

    def test_a_still_camera_is_refused_naming_the_frame(self):
        (self.reference / reference_poses.REFERENCE_POSES).write_text(TRACED)
        self.drawn(STILL)
        with self.assertRaisesRegex(ValueError, "frame 0 stands at .* not at the same pose"):
            reference_poses.check(self.video, self.reference)

    def test_a_turned_camera_at_the_right_place_is_refused(self):
        (self.reference / reference_poses.REFERENCE_POSES).write_text(TRACED)
        self.drawn(HEADER + "0,0,1,2,-3,0,0,0,1\n")
        with self.assertRaisesRegex(ValueError, "frame 0"):
            reference_poses.check(self.video, self.reference)

    def test_skipped_video_frames_line_up_with_the_first_reference(self):
        (self.reference / reference_poses.REFERENCE_POSES).write_text("size 8 8\nlens 0.6 1\npose 4 5 6 1 0 0\n")
        self.drawn(DRAWN)
        reference_poses.check(self.video, self.reference, first=1)

    def test_a_video_without_poses_is_refused_by_a_reference_that_has_them(self):
        (self.reference / reference_poses.REFERENCE_POSES).write_text(TRACED)
        with self.assertRaisesRegex(ValueError, "no .poses.csv beside it"):
            reference_poses.check(self.video, self.reference)

    def test_a_reference_without_poses_is_not_checked(self):
        reference_poses.check(self.video, self.reference)


if __name__ == "__main__":
    unittest.main()
