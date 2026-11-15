"""copy_batches: a renderer's copies packed into SCNE batch rows and read back."""

import math
import pathlib
import random
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from r3d.batch import copy_batches
from r3d.scene_rows import SceneError

# How far a decoded copy may be from its placement: a step of the rows'
# quantisation, in units of the chunk's reach for positions and of one for turns.
POSITION_STEPS = 1.0
TURN_TOLERANCE = 1e-4


def rotation(axis, angle):
    """The rows of a turn by `angle` about unit `axis` (Rodrigues)."""
    x, y, z = axis
    c, s, t = math.cos(angle), math.sin(angle), 1 - math.cos(angle)
    return [[t * x * x + c, t * x * y - s * z, t * x * z + s * y],
            [t * x * y + s * z, t * y * y + c, t * y * z - s * x],
            [t * x * z - s * y, t * y * z + s * x, t * z * z + c]]


def random_axis(rng):
    v = [rng.gauss(0, 1) for _ in range(3)]
    n = math.sqrt(sum(c * c for c in v))
    return [c / n for c in v]


def copies(rng, count, spread=50.0):
    return [{"name": f"c{i}", "matrix": rotation(random_axis(rng), rng.uniform(-math.pi, math.pi)),
             "position": [rng.uniform(-spread, spread) for _ in range(3)], "colour_set": i}
            for i in range(count)]


class CopyBatchesTest(unittest.TestCase):
    def test_every_copy_reads_back_at_its_placement_and_set(self):
        rng = random.Random(7)
        made = copies(rng, 3 * copy_batches.CHUNK_COPIES + 5)
        chunks, places, turns = copy_batches.pack(made)
        self.assertEqual(copy_batches.chunk_count(len(made)), len(chunks))
        by_set = {c["colour_set"]: c for c in made}
        for i, (matrix, position, colour_set) in enumerate(copy_batches.unpack(chunks, places, turns)):
            want = by_set[colour_set]
            reach = chunks[i // copy_batches.CHUNK_COPIES][3]
            step = reach / copy_batches.STEPS
            for a in range(3):
                self.assertLessEqual(abs(position[a] - want["position"][a]), POSITION_STEPS * step + 1e-5)
                for b in range(3):
                    self.assertAlmostEqual(want["matrix"][a][b], matrix[a][b], delta=TURN_TOLERANCE)

    def test_each_chunk_holds_its_copies_within_its_reach(self):
        rng = random.Random(11)
        made = copies(rng, 2 * copy_batches.CHUNK_COPIES + 1)
        chunks, places, _ = copy_batches.pack(made)
        by_set = {c["colour_set"]: c for c in made}
        for i, place in enumerate(places):
            *centre, reach = chunks[i // copy_batches.CHUNK_COPIES]
            self.assertLessEqual(math.dist(centre, by_set[place[3]]["position"]), reach)

    def test_copies_run_in_morton_order_so_a_chunk_is_near_copies(self):
        # Two clusters far apart, interleaved in the input: Morton order keeps
        # each cluster's copies in its own chunks.
        cluster = copy_batches.CHUNK_COPIES
        made = []
        for i in range(2 * cluster):
            x = (0.0 if i % 2 == 0 else 1000.0) + (i // 2) * 0.01
            made.append({"name": f"c{i}", "matrix": rotation([0, 0, 1], 0.0), "position": [x, 0.0, 0.0],
                         "colour_set": i})
        chunks, _, _ = copy_batches.pack(made)
        self.assertEqual(2, len(chunks))
        for chunk in chunks:
            self.assertLess(chunk[3], 1.0)

    def test_a_scaled_or_reflected_copy_is_refused(self):
        turned = rotation([0, 1, 0], 0.5)
        scaled = [[2 * v for v in row] for row in turned]
        reflected = [[-turned[0][0], turned[0][1], turned[0][2]],
                     [-turned[1][0], turned[1][1], turned[1][2]],
                     [-turned[2][0], turned[2][1], turned[2][2]]]
        for matrix in (scaled, reflected):
            with self.assertRaises(SceneError):
                copy_batches.pack([{"name": "bad", "matrix": matrix, "position": [0, 0, 0], "colour_set": 0}])

    def test_a_turn_row_names_its_largest_and_holds_the_rest_in_range(self):
        rng = random.Random(3)
        for _ in range(200):
            *rest, largest = copy_batches.turn_row(rotation(random_axis(rng), rng.uniform(-math.pi, math.pi)))
            self.assertIn(largest, range(4))
            for value in rest:
                self.assertLessEqual(abs(value), copy_batches.STEPS)


if __name__ == "__main__":
    unittest.main()
