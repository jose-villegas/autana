"""The scrub window's state, marks and replay, without a window or a renderer."""
import contextlib
import io
import json
import pathlib
import sys
import tempfile
import unittest

from PIL import Image

TOOLS = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
from render import scene_scrub  # noqa: E402

SCENE = "launcher/demo/room/room.scene.toml"
WIDTH, HEIGHT, FRAMES, DT_MS = 3, 2, 5, 40


def frame(shade):
    """One AVI frame: bottom-up BGR rows padded to four bytes."""
    stride = (WIDTH * 3 + 3) // 4 * 4
    return bytes([shade]) * stride * HEIGHT


def side(label, shade, args=(), bakes=None):
    made = scene_scrub.Side(label, "room", pathlib.Path("."), pathlib.Path("host"), pathlib.Path("assets"),
                            list(args), revision=f"rev-{label}", bakes=dict(bakes or {}), width=WIDTH, height=HEIGHT)
    made.frames = [frame(shade + i) for i in range(FRAMES)]
    made.poses = [scene_scrub.Pose((float(i), 0.0, 0.0), (0.0, 0.0, 0.0, 1.0)) for i in range(FRAMES)]
    return made


def lock(tree, rows):
    path = pathlib.Path(tree) / scene_scrub.BAKES_LOCK
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("".join(f'[[bake]]\noutput = "{output}"\nsource = "{source}"\nsha256 = "{sha}"\n\n'
                            for output, source, sha in rows))


class ScrubState(unittest.TestCase):
    def setUp(self):
        self.scrub = scene_scrub.Scrub(SCENE, "room", DT_MS, [side("fitted", 10), side("source", 100)])

    def test_steps_clamp_to_the_path(self):
        self.scrub.step(-1)
        self.assertEqual(self.scrub.frame, 0)
        self.scrub.step(scene_scrub.FAST_STEP)
        self.assertEqual(self.scrub.frame, FRAMES - 1)
        self.scrub.step(-2)
        self.assertEqual(self.scrub.frame, FRAMES - 3)

    def test_swap_keeps_the_frame_and_names_the_side(self):
        self.scrub.seek(3)
        self.assertEqual(self.scrub.status(), f"frame 3/{FRAMES - 1}  {3 * DT_MS / 1000:.3f} s  A: fitted")
        self.scrub.swap()
        self.assertEqual((self.scrub.frame, self.scrub.shown().label), (3, "source"))
        self.assertTrue(self.scrub.status().endswith("B: source"))
        self.scrub.swap()
        self.assertEqual(self.scrub.shown().label, "fitted")

    def test_the_picture_is_the_shown_sides_frame(self):
        self.scrub.seek(2)
        self.assertEqual(self.scrub.shown().picture(2).getpixel((0, 0)), (12, 12, 12))
        self.scrub.swap()
        picture = scene_scrub.compose(self.scrub)
        self.assertEqual(picture.size, (WIDTH * scene_scrub.ZOOM, HEIGHT * scene_scrub.ZOOM + scene_scrub.STATUS_HEIGHT))
        self.assertEqual(picture.getpixel((0, 0)), (102, 102, 102))

    def test_mark_records_both_sides_and_its_note_lasts_until_the_next_move(self):
        with tempfile.TemporaryDirectory() as folder:
            self.scrub.seek(4)
            path = self.scrub.mark(folder)
            self.assertEqual(path.name, "room-f00004.json")
            self.assertIn(str(path), self.scrub.status())
            record = json.loads(path.read_text())
            self.assertEqual((record["scene"], record["frame"], record["ms"], record["dt_ms"]),
                             (SCENE, 4, 4 * DT_MS, DT_MS))
            for name, label, shade in (("a", "fitted", 14), ("b", "source", 104)):
                marked = record["sides"][name]
                self.assertEqual((marked["label"], marked["revision"]), (label, f"rev-{label}"))
                self.assertEqual(marked["pose"]["position"], [4.0, 0.0, 0.0])
                with Image.open(pathlib.Path(folder) / marked["png"]) as image:
                    self.assertEqual(image.getpixel((0, 0)), (shade, shade, shade))
            self.scrub.step(-1)
            self.assertNotIn("marked", self.scrub.status())


class Bakes(unittest.TestCase):
    def test_only_the_scenes_own_rows(self):
        with tempfile.TemporaryDirectory() as tree:
            lock(tree, [("room.card.mesh", SCENE, "aa"), ("other.mesh", "launcher/demo/x/x.scene.toml", "bb")])
            self.assertEqual(scene_scrub.scene_bakes(tree, SCENE), {"room.card.mesh": "aa"})

    def test_no_lock_is_no_bakes(self):
        with tempfile.TemporaryDirectory() as tree:
            self.assertEqual(scene_scrub.scene_bakes(tree, SCENE), {})


class Replay(unittest.TestCase):
    def mark(self, folder, bakes):
        scrub = scene_scrub.Scrub(SCENE, "room", DT_MS,
                                  [side("a", 0, bakes=bakes), side("b", 0, ["--object", "card"], bakes)])
        scrub.seek(3)
        return scrub.mark(folder)

    def test_replay_renders_up_to_the_marked_frame_at_its_frame_time(self):
        with tempfile.TemporaryDirectory() as folder:
            lock(folder, [("room.card.mesh", SCENE, "aa")])
            path = self.mark(folder, {"room.card.mesh": "aa"})
            warned = io.StringIO()
            with contextlib.redirect_stderr(warned):
                lines = scene_scrub.replay_lines(path, "b", pathlib.Path(folder))
            self.assertEqual(lines, [SCENE, "--object", "card", "--frames", "4", "--dt", str(DT_MS)])
            self.assertEqual(warned.getvalue(), "")

    def test_replay_names_the_bakes_that_changed(self):
        with tempfile.TemporaryDirectory() as folder:
            lock(folder, [("room.card.mesh", SCENE, "new")])
            path = self.mark(folder, {"room.card.mesh": "old", "room.gone.mesh": "cc"})
            warned = io.StringIO()
            with contextlib.redirect_stderr(warned):
                scene_scrub.replay_lines(path, "a", pathlib.Path(folder))
            self.assertIn("room.card.mesh, room.gone.mesh", warned.getvalue())
            self.assertIn("rev-a", warned.getvalue())


if __name__ == "__main__":
    unittest.main()
