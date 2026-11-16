"""Checks how repeated parts of one object grow into units: a window's frame and sill carved as one repeated shape."""

import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

try:
    import numpy as np

    from r3d.objects import carve, units
    from tests.soup import box, concatenate
except ImportError:
    np = None

SCALE = 4  # position ticks per unit, as test_r3d_carve's
FRAME, SILL, PANE = (2.0, 3.0, 0.2), (2.2, 0.2, 0.4), (1.6, 2.6, 0.05)
SILL_AT, PANE_AT = (0.0, -1.7, 0.1), (0.0, 0.0, 0.0)
GLASS = 1  # the pane's material, alpha-tested where a test says so
TURN = np.array([[0., 0., 1.], [0., 1., 0.], [-1., 0., 0.]]) if np is not None else None


def placed(extents, at, turn, origin):
    part = box(extents)
    part.apply_translation(at)
    part.vertices = part.vertices @ turn.T + origin
    return part


def facade(origins, *, sill=None, pane=False, turns=None):
    """One object holding a window per origin: a frame and a sill below it (left out where `sill` is False), and a
    glass pane in the frame when `pane`. Returns (positions, tri_v, tri_m, tri_o, names)."""
    parts, materials = [], []
    for k, origin in enumerate(origins):
        turn = np.eye(3) if turns is None else turns[k]
        pieces = [(FRAME, (0., 0., 0.), 0)]
        if sill is None or sill[k]:
            pieces.append((SILL, SILL_AT, 0))
        if pane:
            pieces.append((PANE, PANE_AT, GLASS))
        for extents, at, material in pieces:
            parts.append(placed(extents, at, turn, origin))
            materials.append(np.full(len(parts[-1].faces), material))
    mesh = concatenate(parts)
    tri_m = np.concatenate(materials)
    return mesh.vertices, mesh.faces, tri_m, np.zeros(len(tri_m), dtype=int), ["facade"]


def row(count, spacing):
    return [np.array([spacing * k, 0., 0.]) for k in range(count)]


@unittest.skipIf(np is None, "numpy is missing")
class UnitTest(unittest.TestCase):
    def grow(self, scene, **kwargs):
        pos, tri_v, tri_m, tri_o, names = scene
        return units.repeated_units(pos, tri_v, tri_m, tri_o, names, **kwargs)

    def unit_parts(self, scene, grown):
        """Per unit object: the sorted triangle counts of the connected parts it holds."""
        pos, tri_v = scene[0], scene[1]
        labels, _ = units.part_labels(pos, tri_v, scene[3])
        return {obj: sorted(np.bincount(labels[grown.tri_o == obj])[np.unique(labels[grown.tri_o == obj])])
                for obj in np.unique(grown.tri_o) if obj >= len(scene[4])}

    def test_a_row_of_windows_grows_one_unit_of_frame_and_sill_per_window(self):
        for spacing in (10.0, 3.0):  # far apart, and closer than a unit's reach
            with self.subTest(spacing=spacing):
                scene = facade(row(4, spacing))
                grown = self.grow(scene)
                self.assertEqual([(u["copies"], u["parts"]) for u in grown.report["units"]], [(4, 2)])
                self.assertEqual(len(grown.object_names), 1 + 4)
                self.assertEqual(set(map(tuple, self.unit_parts(scene, grown).values())), {(12, 12)})

    def test_a_turned_window_joins_in_its_own_frame(self):
        scene = facade(row(4, 10.0), turns=[np.eye(3), TURN, np.eye(3), TURN])
        self.assertEqual([u["copies"] for u in self.grow(scene).report["units"]], [4])

    def test_a_window_without_its_sill_stays_in_the_object(self):
        scene = facade(row(4, 10.0), sill=[True, True, False, True])
        grown = self.grow(scene)
        self.assertEqual([(u["copies"], u["parts"]) for u in grown.report["units"]], [(3, 2)])
        # The sill-less frame keeps the facade's id.
        self.assertEqual(int((grown.tri_o == 0).sum()), 12)

    def test_too_few_full_copies_grow_no_unit(self):
        grown = self.grow(facade(row(3, 10.0), sill=[True, True, False]))
        self.assertEqual(grown.report["units"], [])
        self.assertTrue((grown.tri_o == 0).all())

    def test_an_alpha_tested_pane_stays_in_the_object(self):
        scene = facade(row(4, 10.0), pane=True)
        grown = self.grow(scene, masked=[GLASS])
        self.assertEqual([(u["copies"], u["parts"]) for u in grown.report["units"]], [(4, 2)])
        self.assertTrue((grown.tri_o[scene[2] == GLASS] == 0).all())
        opaque = self.grow(scene)
        self.assertEqual([(u["copies"], u["parts"]) for u in opaque.report["units"]], [(4, 3)])

    def test_a_taken_object_grows_no_unit(self):
        grown = self.grow(facade(row(4, 10.0)), taken={0})
        self.assertEqual(grown.report["units"], [])

    def test_units_repeat_as_objects(self):
        scene = facade(row(4, 10.0))
        grown = self.grow(scene)
        found, _ = carve.repeats(scene[0], scene[1], scene[2], grown.tri_o, grown.object_names, {}, SCALE)
        self.assertEqual(sorted(found), [1, 2, 3, 4])
        self.assertTrue(all(rule is carve.FREE_REPEAT for rule in found.values()))


if __name__ == "__main__":
    unittest.main()
