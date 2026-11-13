"""LTEX and LUVS writers: layouts round trip, the pattern averages one on screen, planar UVs are exact."""
import pathlib
import sys
import types
import unittest

import numpy as np

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from r3d.texture import texture_asset as ltex
from r3d.texture import uv_transfer as luvs

SIZE = 8
CHANNELS = ltex.TINT_RGB
# Display values the palette may stray from ONE by after 16-colour quantization of a pattern.
ONE_TOLERANCE = ltex.ONE * 0.08


def display_to_linear(display):
    return np.asarray(display, dtype=np.float64) ** ltex.GAMMA


def flat_source(display, side=16):
    return np.full((side, side, CHANNELS), display_to_linear(display))


def checker_source(dark, light, side=16):
    """Linear (side, side, 3) checker of two display colours, 2 texel squares."""
    cell = (np.indices((side, side)) // 2).sum(axis=0) % 2
    colours = np.stack([np.full(CHANNELS, display_to_linear(dark)), np.full(CHANNELS, display_to_linear(light))])
    return colours[cell]


def simple_indexed(side=4, levels=2):
    palette = [(i * 3, i * 5 + 1, 255 - i) for i in range(ltex.COLOURS)]
    rows = [[(x + y + level) % ltex.COLOURS for x in range(side >> level)] for level in range(levels)
            for y in range(side >> level)]
    out, at = [], 0
    for level in range(levels):
        out.append(rows[at:at + (side >> level)])
        at += side >> level
    return ltex.Indexed((10, 20, 30), palette, out)


def level_palette_values(indexed, level):
    palette = np.asarray(indexed.palette, dtype=np.float64)
    return palette[np.asarray(indexed.levels[level])]


class LtexTests(unittest.TestCase):
    def test_round_trip(self):
        indexed = simple_indexed()
        decoded = ltex.decode(ltex.pack_ltex(indexed))
        self.assertEqual(decoded.tint, indexed.tint)
        self.assertEqual(decoded.palette, indexed.palette)
        self.assertEqual(decoded.levels, indexed.levels)

    def test_entry_length(self):
        indexed = simple_indexed(side=8, levels=3)
        texel_bytes = sum(len(rows) * len(rows[0]) // 2 for rows in indexed.levels)
        entry = ltex.pack_ltex(indexed)
        self.assertEqual(len(entry), ltex.HEADER.size + ltex.COLOURS * ltex.PALETTE_STRIDE + texel_bytes)

    def test_decode_rejects_bad_headers(self):
        entry = ltex.pack_ltex(simple_indexed(side=4, levels=2))
        fields = list(ltex.HEADER.unpack_from(entry))
        body = entry[ltex.HEADER.size:]
        names = [name for name, _ in ltex.HEADER_FIELDS]

        def with_field(name, value):
            changed = list(fields)
            changed[names.index(name)] = value
            return ltex.HEADER.pack(*changed) + body

        log2 = fields[names.index("SIZE_LOG2")]
        for broken in (with_field("VERSION", ltex.VERSION + 1),
                       with_field("FORMAT", ltex.INDEX4 + 1),
                       with_field("LEVELS", log2 - ltex.SIZE_LOG2_MIN + 2),
                       with_field("LEVELS", 0)):
            with self.assertRaises(ValueError):
                ltex.decode(broken)

    def test_decode_rejects_wrong_length(self):
        entry = ltex.pack_ltex(simple_indexed())
        for broken in (entry[:-1], entry + b"\0"):
            with self.assertRaises(ValueError):
                ltex.decode(broken)

    def test_size_log2_of(self):
        for log2 in range(ltex.SIZE_LOG2_MIN, ltex.SIZE_LOG2_MAX + 1):
            self.assertEqual(ltex.size_log2_of(1 << log2), log2)
        bad = [0, 3, 6, (1 << ltex.SIZE_LOG2_MIN) + 1, 1 << (ltex.SIZE_LOG2_MIN - 1),
               1 << (ltex.SIZE_LOG2_MAX + 1), 3 << ltex.SIZE_LOG2_MAX]
        for size in bad:
            with self.subTest(size=size), self.assertRaises(ValueError):
                ltex.size_log2_of(size)


class MaterialTextureTests(unittest.TestCase):
    def test_flat_source_is_one_everywhere(self):
        indexed = ltex.material_texture(flat_source(0.4), SIZE)
        for level in range(len(indexed.levels)):
            values = level_palette_values(indexed, level)
            self.assertTrue(np.all(np.abs(values - ltex.ONE) <= ONE_TOLERANCE), f"level {level}")

    def test_checker_averages_one(self):
        indexed = ltex.material_texture(checker_source(0.2, 0.8), SIZE)
        mean = level_palette_values(indexed, 0).reshape(-1, CHANNELS).mean(axis=0)
        self.assertTrue(np.all(np.abs(mean - ltex.ONE) <= ONE_TOLERANCE), mean)
        # The pattern is not flat: dark and light squares differ.
        values = level_palette_values(indexed, 0)
        self.assertGreater(values.max() - values.min(), ltex.ONE * 0.5)

    def test_levels_halve(self):
        indexed = ltex.material_texture(checker_source(0.2, 0.8), SIZE)
        count = SIZE.bit_length() - 1 - ltex.SIZE_LOG2_MIN + 1
        self.assertEqual(len(indexed.levels), count)
        for level, rows in enumerate(indexed.levels):
            self.assertEqual(len(rows), SIZE >> level)
            self.assertTrue(all(len(row) == SIZE >> level for row in rows))

    def test_default_tint_is_display_mean(self):
        display = np.array([0.2, 0.5, 0.8])
        source = np.full((16, 16, CHANNELS), display_to_linear(display))
        indexed = ltex.material_texture(source, SIZE)
        self.assertEqual(indexed.tint, tuple(int(round(c)) for c in display * ltex.BYTE_MAX))

    def test_explicit_tint_is_rounded(self):
        indexed = ltex.material_texture(flat_source(0.4), SIZE, tint=(10.4, 20.6, 255.0))
        self.assertEqual(indexed.tint, (10, 21, 255))

    def test_rejects_bad_size(self):
        with self.assertRaises(ValueError):
            ltex.material_texture(flat_source(0.4), 6)


class LuvsTests(unittest.TestCase):
    def test_round_trip(self):
        # Three vertices, then the first cluster's two again on chart 1.
        value = luvs.Luvs(np.array([[1, -2], [300, -400], [5, 6], [7, 8], [-9, 10]], dtype=np.int16),
                          np.array([[[0, 1], [4, -5]], [[-3, 2], [0, 0]]], dtype=np.int16),
                          np.array([3, 0], dtype=np.int32),
                          np.array([[0, 1], [1, luvs.NONE]], dtype=np.uint8),
                          np.array([1, 0, 2, 1, 0, 0, 0, 0, 2], dtype=np.uint8), 3, 2)
        decoded = luvs.decode(luvs.pack_luvs(value))
        for field in ("uvs", "origins", "shifts", "materials", "codes"):
            np.testing.assert_array_equal(getattr(decoded, field), getattr(value, field), field)
        self.assertEqual((decoded.vertex_count, decoded.material_count), (3, 2))

    def test_decode_rejects_bad_entries(self):
        value = luvs.Luvs(np.zeros((3, 2), dtype=np.int16), np.zeros((1, luvs.CHARTS, 2), dtype=np.int16),
                          np.zeros(1, dtype=np.int32), np.zeros((1, luvs.CHARTS), dtype=np.uint8),
                          np.ones(2, dtype=np.uint8), 3, 1)
        entry = luvs.pack_luvs(value)
        fields = list(luvs.HEADER.unpack_from(entry))
        fields[0] = luvs.VERSION + 1
        for broken in (luvs.HEADER.pack(*fields) + entry[luvs.HEADER.size:], entry[:-1], entry + b"\0"):
            with self.assertRaises(ValueError):
                luvs.decode(broken)


class PlaneUvTests(unittest.TestCase):
    CORNERS = np.array([[0.0, 0.0, 0.0], [2.0, 0.0, 0.0], [0.0, 3.0, 0.0]])

    def affine(self, points):
        """A planar mapping: u = 4x + 1, v = 2y - 3 (on a plane z = 0)."""
        return np.stack([4 * points[:, 0] + 1, 2 * points[:, 1] - 3], axis=1)

    def test_exact_on_and_outside_triangle(self):
        uv = self.affine(self.CORNERS)
        points = np.array([[0.5, 0.5, 0.0], [1.0, 0.0, 0.0], [5.0, -2.0, 0.0], [-3.0, 7.0, 0.0]])
        np.testing.assert_allclose(luvs.plane_uv(points, self.CORNERS, uv), self.affine(points), atol=1e-9)

    def test_projects_off_plane_points(self):
        uv = self.affine(self.CORNERS)
        points = np.array([[0.5, 0.5, 4.0]])
        np.testing.assert_allclose(luvs.plane_uv(points, self.CORNERS, uv), self.affine(points), atol=1e-9)

    def test_degenerate_triangle_is_none(self):
        line = np.array([[0.0, 0.0, 0.0], [1.0, 0.0, 0.0], [2.0, 0.0, 0.0]])
        point = np.zeros((3, 3))
        for corners in (line, point):
            self.assertIsNone(luvs.plane_uv(np.array([[0.5, 0.5, 0.0]]), corners, np.zeros((3, 2))))


def fake_scene(textured=(True, False), names=("Brick_a", "Glass"), materials=(0, 0)):
    """A unit quad in z = 0 (two triangles, of `materials`), mapped over the whole texture, and the same baked
    mesh."""
    corners = np.array([[0.0, 0.0, 0.0], [1.0, 0.0, 0.0], [1.0, 1.0, 0.0], [0.0, 1.0, 0.0]])
    tris = np.array([[0, 1, 2], [0, 2, 3]])
    texture = types.SimpleNamespace(levels=[checker_source(0.2, 0.8)])
    source = types.SimpleNamespace(
        names=list(names), textures=[texture if t else None for t in textured], p=corners, tri_v=tris,
        tri_m=np.array(materials), uv=corners[:, :2].copy(), tri_t=tris)
    scale = 100
    mesh = types.SimpleNamespace(
        pos=corners * scale, position_scale=scale, tris=tris, clusters=[(0, 4, 0, 2, 0, 0, 0, 0, 0, 0)],
        rgb=np.full((4, 3), 90.0))
    return mesh, source


class TexturedEntriesTests(unittest.TestCase):
    def test_unmatched_pattern_names_it(self):
        mesh, source = fake_scene()
        with self.assertRaisesRegex(ValueError, "Missing"):
            luvs.textured_entries(mesh, "m", source, ["Brick_*", "Missing"], SIZE)

    def test_pattern_only_matching_untextured_material_raises(self):
        mesh, source = fake_scene()
        with self.assertRaisesRegex(ValueError, "Glass"):
            luvs.textured_entries(mesh, "m", source, ["Glass"], SIZE)

    def test_rows_for_matched_materials(self):
        mesh, source = fake_scene()
        rows = luvs.textured_entries(mesh, "m", source, ["Brick_*"], SIZE)
        self.assertEqual([(name, kind) for name, kind, _ in rows],
                         [(luvs.texture_name("m", 0), ltex.TYPE), (luvs.name_for_mesh("m"), luvs.TYPE)])
        texture = ltex.decode(rows[0][2])
        self.assertEqual(len(texture.levels[0]), SIZE)
        # The LTEX tint is the textured triangles' mean vertex colour.
        self.assertEqual(texture.tint, (90, 90, 90))
        decoded = luvs.decode(rows[1][2])
        self.assertEqual(decoded.material_count, 1)
        self.assertEqual(decoded.materials.tolist(), [[0, luvs.NONE]])
        self.assertEqual(len(decoded.uvs), 4)

    def test_cluster_uvs_follow_planar_mapping(self):
        mesh, source = fake_scene()
        rows = luvs.textured_entries(mesh, "m", source, ["Brick_*"], SIZE)
        decoded = luvs.decode(rows[-1][2])
        origin = decoded.origins[0, 0].astype(np.int64)
        texels = (decoded.uvs.astype(np.float64) / (1 << luvs.UV_SHIFT)) + origin * SIZE
        corners = source.p[:, :2]
        expected = np.stack([corners[:, 0] * SIZE, (1.0 - corners[:, 1]) * SIZE], axis=1)
        np.testing.assert_allclose(texels, expected, atol=1.0 / (1 << luvs.UV_SHIFT))

    def test_triangles_on_another_material_draw_plain(self):
        mesh, source = fake_scene(materials=(0, 1))
        decoded = luvs.decode(luvs.textured_entries(mesh, "m", source, ["Brick_*"], SIZE)[-1][2])
        self.assertEqual(decoded.materials.tolist(), [[0, luvs.NONE]])
        self.assertEqual(list(decoded.codes), [1, luvs.PLAIN])

    def test_triangle_off_chart_0s_plane_takes_chart_1(self):
        mesh, source = fake_scene()
        # The second triangle folded up out of the first's plane, in the baked mesh and the source alike, so both
        # keep their material; the folded one is the larger, so its plane is chart 0 and the flat one takes chart 1,
        # its UVs after the vertices' and its mapping its own source triangle's.
        mesh.pos = mesh.pos.copy()
        mesh.pos[3] = [0.0, 0.0, 100.0]
        source.p = source.p.copy()
        source.p[3] = [0.0, 0.0, 1.0]
        decoded = luvs.decode(luvs.textured_entries(mesh, "m", source, ["Brick_*"], SIZE)[-1][2])
        self.assertEqual(list(decoded.codes), [2, 1])
        self.assertEqual(decoded.materials.tolist(), [[0, 0]])
        self.assertEqual((len(decoded.uvs), int(decoded.shifts[0])), (8, 4))
        on_1 = decoded.uvs[4:].astype(np.float64) / (1 << luvs.UV_SHIFT) + decoded.origins[0, 1] * SIZE
        flat = source.p[:3, :2]
        np.testing.assert_allclose(on_1[:3], np.stack([flat[:, 0] * SIZE, (1.0 - flat[:, 1]) * SIZE], axis=1),
                                   atol=1.0 / (1 << luvs.UV_SHIFT))

    def test_a_cluster_takes_at_most_its_charts(self):
        mesh, source = fake_scene()
        # Folded as above, but the source keeps the quad flat: the folded triangle faces no source triangle, so it
        # never textures, and the flat one alone takes chart 0.
        mesh.pos = mesh.pos.copy()
        mesh.pos[3] = [0.0, 0.0, 100.0]
        decoded = luvs.decode(luvs.textured_entries(mesh, "m", source, ["Brick_*"], SIZE)[-1][2])
        self.assertEqual(list(decoded.codes), [1, luvs.PLAIN])
        self.assertEqual(decoded.materials.tolist(), [[0, luvs.NONE]])
        self.assertEqual(len(decoded.uvs), 4)

    def test_matched(self):
        names = ["Brick_a", "Brick_b", "Glass"]
        self.assertEqual(luvs.matched(names, ["Brick_*"]), [0, 1])
        self.assertEqual(luvs.matched(names, ["Glass", "Brick_b"]), [1, 2])
        self.assertEqual(luvs.matched(names, ["brick_*"]), [])


if __name__ == "__main__":
    unittest.main()
