"""torus_obj.py: a closed torus, every face counter-clockwise from outside.

    python -m unittest discover -s launcher/main/apps/render_lab/tools/tests -p test_torus_obj.py
"""
import collections
import math
import pathlib
import sys
import tempfile
import unittest
from unittest import mock

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
from torus_obj import main, torus  # noqa: E402
from r3d.obj import load_obj  # noqa: E402  (torus_obj puts launcher/tools on the path)

RING_RADIUS = 4.0
TUBE_RADIUS = 1.0
RING_STEPS = 6
TUBE_STEPS = 4
STEM = "ring"


def signed_volume(vertices, faces):
    """The volume the faces enclose, positive when each faces away from the inside."""
    total = 0.0
    for a, b, c in faces:
        (ax, ay, az), (bx, by, bz), (cx, cy, cz) = vertices[a], vertices[b], vertices[c]
        total += ax * (by * cz - bz * cy) - ay * (bx * cz - bz * cx) + az * (bx * cy - by * cx)
    return total / 6.0


class TorusObj(unittest.TestCase):
    def setUp(self):
        self.vertices, self.faces = torus(RING_RADIUS, TUBE_RADIUS, RING_STEPS, TUBE_STEPS)

    def test_one_vertex_per_step_pair_and_two_triangles(self):
        self.assertEqual(RING_STEPS * TUBE_STEPS, len(self.vertices))
        self.assertEqual(2 * RING_STEPS * TUBE_STEPS, len(self.faces))
        for face in self.faces:
            for corner in face:
                self.assertIn(corner, range(len(self.vertices)))

    def test_closed_and_consistently_wound(self):
        """Each edge is walked once each way: no hole where either step wraps, and no face flipped."""
        directed = collections.Counter((a, b) for f in self.faces for a, b in zip(f, f[1:] + f[:1]))
        self.assertTrue(all(n == 1 for n in directed.values()), "an edge is walked twice the same way")
        self.assertTrue(all((b, a) in directed for a, b in directed), "an edge has only one face")

    def test_faces_point_outward(self):
        """The enclosed volume is the torus's, 2 pi^2 R r^2, positive: each face faces away from the tube."""
        whole = 2.0 * math.pi ** 2 * RING_RADIUS * TUBE_RADIUS ** 2
        volume = signed_volume(self.vertices, self.faces)
        self.assertGreater(volume, 0.0)
        self.assertLess(volume, whole)  # the coarse polygon cuts inside the smooth tube


class TorusMain(unittest.TestCase):
    def test_main_writes_a_banner_its_own_material_and_the_torus_for_load_obj(self):
        with tempfile.TemporaryDirectory() as folder:
            out = pathlib.Path(folder) / f"{STEM}.obj"
            arguments = [f"{RING_RADIUS:g}", f"{TUBE_RADIUS:g}", str(RING_STEPS), str(TUBE_STEPS)]
            with mock.patch.object(sys, "argv", ["torus_obj.py", str(out), *arguments]):
                main()
            text = out.read_bytes().decode()
            positions, _, tri_v, _, tri_m, materials, _, objects = load_obj(out)
        command = f"python main/apps/render_lab/tools/torus_obj.py {out.as_posix()} {' '.join(arguments)}"
        banner = ["# GENERATED FILE - do not edit.", "#", f"#     {command}", "#", f"mtllib {STEM}.mtl", "v "]
        head = "\n".join(banner)
        self.assertTrue(text.startswith(head), text[:len(head) + 40])
        self.assertIn(f"\nusemtl {STEM}\n", text)
        self.assertEqual(RING_STEPS * TUBE_STEPS, len(positions))
        self.assertEqual(2 * RING_STEPS * TUBE_STEPS, len(tri_v))
        self.assertEqual([STEM], materials)
        self.assertEqual({0}, set(tri_m.tolist()))
        self.assertEqual([STEM], objects)  # no o line: the file's own name


if __name__ == "__main__":
    unittest.main()
