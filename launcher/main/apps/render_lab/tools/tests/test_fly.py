"""fly.py: the lines it sends are the ones render_lab_fly.h reads, and it sends them often enough to keep a flight."""
import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
import fly  # noqa: E402


class SteerLine(unittest.TestCase):
    def test_keys_map_to_their_axes_and_opposites_cancel(self):
        self.assertEqual(fly.steer_line({"w", "d", "e"}, 0, 0, 0), "render fly 1 1 1 0 0 0")
        self.assertEqual(fly.steer_line({"s", "a", "q"}, 0, 0, 0), "render fly -1 -1 -1 0 0 0")
        self.assertEqual(fly.steer_line({"w", "s"}, 0, 0, 0), "render fly 0 0 0 0 0 0")

    def test_keys_that_steer_nothing_are_ignored(self):
        self.assertEqual(fly.steer_line({"p", "shift_l"}, 0, 0, 0), "render fly 0 0 0 0 0 0")

    def test_the_line_carries_the_drag_and_wheel_after_the_axes(self):
        self.assertEqual(fly.steer_line(set(), -12, 7, -2), "render fly 0 0 0 -12 7 -2")
        self.assertEqual(len(fly.steer_line(set(), 0, 0, 0).split()) - 2, fly.AXES + 3)

    def test_part_pixels_and_notches_wait_for_the_next_line(self):
        gathered = fly.Gathered()
        gathered.drag_x, gathered.drag_y, gathered.wheel = 2.5, -1.5, 0.5
        self.assertEqual(gathered.take(), (2, -1, 0))
        gathered.drag_x += 0.5
        gathered.wheel += 0.5
        self.assertEqual(gathered.take(), (1, 0, 1))

    def test_lines_come_well_inside_the_boards_idle_limit(self):
        self.assertLess(fly.SEND_MS * 4, fly.IDLE_MS)


if __name__ == "__main__":
    unittest.main()
