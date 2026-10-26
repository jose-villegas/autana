"""Checks reduce_source's simplification of an exported OBJ, without Blender."""

import pathlib
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

try:
    import numpy as np

    from r3d.obj import load_obj
    from r3d.source import reduce_source
except ImportError:
    np = None

GRID = 10  # quads per side of the flat object
CARD = 1.0  # the small object's side, in model units


def write_source(directory):
    """NAME.obj with two objects: a flat GRID x GRID sheet of material `a` and a
    CARD-sized square of material `leaf`."""
    lines = ["mtllib m.mtl", "vt 0 0"]
    for z in range(GRID + 1):
        for x in range(GRID + 1):
            lines.append(f"v {x} 0 {z}")
    card = (GRID + 1) ** 2
    lines += ["v 20 0 0", f"v {20 + CARD} 0 0", f"v {20 + CARD} 0 {CARD}", f"v 20 0 {CARD}"]
    lines += ["o sheet", "usemtl a"]
    for z in range(GRID):
        for x in range(GRID):
            i = z * (GRID + 1) + x + 1
            lines += [f"f {i}/1 {i + GRID + 1}/1 {i + 1}/1", f"f {i + 1}/1 {i + GRID + 1}/1 {i + GRID + 2}/1"]
    lines += ["o card", "usemtl leaf", f"f {card + 1}/1 {card + 4}/1 {card + 3}/1",
              f"f {card + 1}/1 {card + 3}/1 {card + 2}/1"]
    (directory / "m.obj").write_text("\n".join(lines) + "\n")
    (directory / "m.mtl").write_text("newmtl a\nKd 1 1 1\nnewmtl leaf\nKd 0 1 0\n")


def groups(path):
    """{object name: [material of each triangle]} of an OBJ, in file order."""
    found, current, material = {}, None, None
    for line in path.read_text().splitlines():
        key, _, value = line.partition(" ")
        if key == "o":
            current = found.setdefault(value, [])
        elif key == "usemtl":
            material = value
        elif key == "f":
            current.append(material)
    return found


@unittest.skipIf(np is None, "needs the r3d environment")
class ReduceSourceTest(unittest.TestCase):
    def reduce(self, error, error_for=()):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        root = pathlib.Path(directory.name)
        write_source(root)
        counts = reduce_source.reduce(root, "m", root / "out", error, error_for)
        return root, counts

    def test_each_object_stays_its_own_group_with_its_material(self):
        root, _ = self.reduce(0.01)
        found = groups(root / "out" / "m.obj")
        self.assertEqual(set(found), {"sheet", "card"})
        self.assertEqual(set(found["sheet"]), {"a"})
        self.assertEqual(found["card"], ["leaf", "leaf"])

    def test_a_flat_sheet_collapses_and_every_corner_is_a_source_vertex(self):
        root, (before, after) = self.reduce(0.01)
        self.assertEqual(before, 2 * GRID * GRID + 2)
        self.assertLess(len(groups(root / "out" / "m.obj")["sheet"]), GRID)
        source = load_obj(str(root / "m.obj"))[0]
        reduced = load_obj(str(root / "out" / "m.obj"))[0]
        self.assertTrue(all((np.abs(source - p).max(axis=1) < 1e-3).any() for p in reduced))
        self.assertEqual(after, sum(len(t) for t in groups(root / "out" / "m.obj").values()))

    def test_a_coarser_error_for_a_material_prunes_its_small_pieces_alone(self):
        root, _ = self.reduce(0.01, ["leaf=" + str(4 * CARD)])
        self.assertEqual(set(groups(root / "out" / "m.obj")), {"sheet"})

    def test_the_last_matching_rule_sets_a_materials_error(self):
        errors = reduce_source.material_errors(["a", "leaf", "leaf_b"], 1.0, ["leaf*=4", "leaf_b=9"])
        self.assertEqual(errors.tolist(), [1.0, 4.0, 9.0])


if __name__ == "__main__":
    unittest.main()
