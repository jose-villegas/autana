"""reduce_source keeps rigid copies exact, so the import finds them again."""

import pathlib
import sys
import tempfile
import unittest

import numpy as np
from scipy.spatial.transform import Rotation

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from r3d.obj import load_obj
from r3d.objects.instances import match_instances, source_objects
from r3d.source.reduce_source import reduce
from soup import sphere


def write_source(directory, objects):
    """NAME.obj with one `o` group per (name, positions, faces), one material, and its .mtl."""
    lines, base = ["mtllib copies.mtl", "vt 0 0"], 1
    for _, positions, _ in objects:
        lines += [f"v {x:.6f} {y:.6f} {z:.6f}" for x, y, z in positions]
    for name, positions, faces in objects:
        lines += [f"o {name}", "usemtl stone"] + [f"f {a}/1 {b}/1 {c}/1" for a, b, c in faces + base]
        base += len(positions)
    (directory / "copies.obj").write_text("\n".join(lines) + "\n")
    (directory / "copies.mtl").write_text("newmtl stone\nKd 0.5 0.5 0.5\n")


class ReduceCopiesTests(unittest.TestCase):
    def test_copies_are_reduced_once_and_placed_exactly(self):
        points, faces = sphere(3)
        corners = points[faces]
        outward = np.einsum("ij,ij->i", np.cross(corners[:, 1] - corners[:, 0], corners[:, 2] - corners[:, 0]),
                            corners.sum(axis=1)) > 0
        faces = np.where(outward[:, None], faces, faces[:, ::-1])
        lump = points * [3.0, 2.0, 1.0]
        turn = Rotation.from_euler("xyz", [20, 70, -35], degrees=True).as_matrix()
        mirror = np.diag([-1.0, 1.0, 1.0])
        objects = [("first", lump + 10, faces), ("turned", lump @ turn.T - [5, 0, 2], faces),
                   ("mirrored", lump @ mirror.T + [0, 8, 0], faces[:, ::-1])]
        with tempfile.TemporaryDirectory() as directory:
            source, out = pathlib.Path(directory) / "in", pathlib.Path(directory) / "out"
            source.mkdir()
            write_source(source, objects)
            before, after = reduce(source, "copies", out, 0.8)
            p, _, tri_v, _, tri_m, _, tri_o, names = load_obj(str(out / "copies.obj"))
        self.assertEqual(before, 3 * len(faces))
        counts = np.bincount(tri_o)
        self.assertEqual(len(set(counts)), 1)
        self.assertLess(counts[0], len(faces))
        self.assertEqual(after, 3 * counts[0])
        shapes = match_instances(source_objects(p, tri_v, tri_m, tri_o, names))
        self.assertEqual([[i.object_name for i in s.instances] for s in shapes], [["first", "turned", "mirrored"]])
        self.assertEqual([i.mirrored for i in shapes[0].instances], [False, False, True])


if __name__ == "__main__":
    unittest.main()
