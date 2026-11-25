"""Checks the alpha cutout (r3d/light.py, r3d/ray_query.py) on a checkered card above a plane: AlphaTest reads
the texture at each hit's UV, every filtered query method passes the cut texels, and drop_masked keeps the
triangles its mean alpha keeps."""

import pathlib
import sys
import tempfile
import unittest
from copy import copy
from types import SimpleNamespace
from unittest import mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

try:
    import numpy as np
    from PIL import Image

    from r3d import light
    from r3d.obj import ALPHA_OPAQUE, Texture, alpha_tested
except ImportError:
    np = None

from tests.r3d_env import needs_mitsuba  # noqa: E402
from tests.r3d_env import have_llvm  # noqa: E402

CARD_Z, PLANE_Z, EYE_Z = 1.0, 0.0, 3.0
HALF = 1.0
KEEP_ALPHA = 0.5
TEXELS = 8
# Where the rays cross the card, in its UVs: two in opaque checker quarters, two in cut ones.
RAY_UV = ((0.2, 0.3), (0.7, 0.3), (0.2, 0.8), (0.7, 0.8))
SOLID = [True, False, False, True]


def quad(z, half):
    return [[-half, -half, z], [half, -half, z], [half, half, z], [-half, half, z]]


def checker_source(directory, plane=True):
    """A card at CARD_Z whose texture is a 2x2 checker of opaque and cut quarters, above an untextured plane."""
    y, x = np.indices((TEXELS, TEXELS))
    pixels = np.full((TEXELS, TEXELS, 4), 255, dtype=np.uint8)
    pixels[..., 3] = ((x // (TEXELS // 2) + y // (TEXELS // 2)) % 2) * 255
    path = pathlib.Path(directory) / "checker.png"
    Image.fromarray(pixels).save(path)
    quads = [quad(CARD_Z, HALF)] + ([quad(PLANE_Z, 2 * HALF)] if plane else [])
    corners = np.array([corner for q in quads for corner in q], dtype=float)
    tris = np.array([[b, b + 1, b + 2] for b in range(0, len(corners), 4)] +
                    [[b, b + 2, b + 3] for b in range(0, len(corners), 4)])
    tris = tris[np.argsort(tris[:, 0], kind="stable")]
    uv = np.tile([[0.0, 0.0], [1.0, 0.0], [1.0, 1.0], [0.0, 1.0]], (len(quads), 1))
    return SimpleNamespace(p=corners, uv=uv, tri_v=tris, tri_t=tris, tri_m=np.repeat(np.arange(len(quads)), 2),
                           textures=[Texture(path)] + [None] * (len(quads) - 1))


def checker_rays():
    xy = (np.array(RAY_UV) * 2 - 1) * HALF
    origins = np.column_stack([xy, np.full(len(xy), EYE_Z)])
    return origins, np.tile([0.0, 0.0, -1.0], (len(xy), 1))


@unittest.skipIf(np is None, "the r3d environment is not installed")
class AlphaTestTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.directory = directory.name
        self.source = checker_source(self.directory)

    def card_hits(self):
        origins, directions = checker_rays()
        locations = origins + directions * (EYE_Z - CARD_Z)
        # Below the card's diagonal is its first triangle, above it its second.
        return np.array([0 if u > v else 1 for u, v in RAY_UV]), locations

    def test_a_hit_is_solid_where_the_texel_at_its_uv_is_kept(self):
        triangles, locations = self.card_hits()
        np.testing.assert_array_equal(light.AlphaTest(self.source, KEEP_ALPHA)(triangles, locations), SOLID)
        self.source.uv += [2.0, -1.0]
        np.testing.assert_array_equal(light.AlphaTest(self.source, KEEP_ALPHA)(triangles, locations), SOLID,
                                      "texture coordinates outside 0..1 should repeat")

    def test_opaque_and_untextured_materials_are_solid_without_a_sample(self):
        triangles, locations = self.card_hits()
        opaque = copy(self.source.textures[0])
        opaque.levels = [np.ones_like(level) for level in opaque.levels]
        self.source.textures[1] = opaque
        alpha = light.AlphaTest(self.source, KEEP_ALPHA)
        with mock.patch.object(opaque, "sample", side_effect=AssertionError("opaque texture sampled")):
            np.testing.assert_array_equal(alpha(triangles + 2, locations), True)
        self.source.textures[1] = None
        np.testing.assert_array_equal(light.AlphaTest(self.source, KEEP_ALPHA)(triangles + 2, locations), True)
        np.testing.assert_array_equal(alpha(np.zeros(0, dtype=int), np.zeros((0, 3))), [])

    def test_opaque_colour_is_sampled_and_untextured_hits_are_white(self):
        hits, points = self.card_hits()
        colour = np.array([[[0.2, 0.4, 0.6, 1.0]]])
        texture = Texture.from_levels([colour])
        self.source.textures[0] = texture
        alpha = light.AlphaTest(self.source, KEEP_ALPHA)
        with mock.patch.object(texture, "sample", wraps=texture.sample) as sample:
            actual = alpha.rgba(hits, points)
        self.assertEqual(sample.call_count, 1)
        np.testing.assert_allclose(actual, np.repeat(colour[0], len(hits), axis=0))
        self.source.textures[0] = None
        np.testing.assert_array_equal(alpha.rgba(hits, points), 1.)

    def test_a_material_is_alpha_tested_when_any_texel_is_at_or_under_the_opaque_line(self):
        def texture(alpha_min):
            level = np.ones((2, 2, 4))
            level[0, 0, 3] = alpha_min
            return SimpleNamespace(levels=[level])

        tested = alpha_tested([None, texture(1.0), texture(ALPHA_OPAQUE), texture(ALPHA_OPAQUE + 0.005)])
        np.testing.assert_array_equal(tested, [False, False, True, False])

    def test_one_texel_decides_at_level_zero_and_keep_alpha_itself_is_solid(self):
        texture = copy(self.source.textures[0])
        level = np.ones((TEXELS, TEXELS, 4))
        row, column = 2, 5
        u, v = (column + 0.5) / TEXELS, 1.0 - (row + 0.5) / TEXELS
        location = np.array([[u * 2 * HALF - HALF, v * 2 * HALF - HALF, CARD_Z]])
        triangle = np.array([0 if u > v else 1])
        for alpha, solid in ((KEEP_ALPHA, True), (KEEP_ALPHA / 2, False)):
            level[row, column, 3] = alpha
            # The coarser level averages the texel away; only level 0 still cuts it.
            texture.levels = [level.copy(), np.ones((TEXELS // 2, TEXELS // 2, 4))]
            self.source.textures[0] = texture
            with self.subTest(alpha=alpha):
                np.testing.assert_array_equal(light.AlphaTest(self.source, KEEP_ALPHA)(triangle, location), [solid])

    def test_drop_masked_keeps_the_triangles_mostly_opaque(self):
        source = checker_source(self.directory, plane=False)
        pixels = np.full((TEXELS, TEXELS, 4), 255, dtype=np.uint8)
        pixels[:, : TEXELS // 2, 3] = 0
        path = pathlib.Path(self.directory) / "edge.png"
        Image.fromarray(pixels).save(path)
        source.textures[0] = Texture(path)
        # Cut the card's left half: the triangle along its left edge averages under KEEP_ALPHA, the other over.
        alpha = light.triangle_alpha(source.uv, source.tri_t, source.textures[0])
        self.assertLess(alpha[1], KEEP_ALPHA)
        self.assertGreater(alpha[0], KEEP_ALPHA)
        tri_v, _, _, keep = light.drop_masked(source.p, source.uv, source.tri_v, source.tri_t, source.tri_m,
                                              source.textures, KEEP_ALPHA)
        np.testing.assert_array_equal(keep, alpha >= KEEP_ALPHA)
        np.testing.assert_array_equal(tri_v, source.tri_v[keep])


@needs_mitsuba
@unittest.skipIf(np is None, "the r3d environment is not installed")
class FilteredQueryTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.directory = directory.name

    def query(self, variant, plane=True):
        from r3d.ray_query import RayQuery

        source = checker_source(self.directory, plane)
        return RayQuery(source.p, source.tri_v, variant, hit_filter=light.AlphaTest(source, KEEP_ALPHA))

    def test_every_query_method_passes_the_cut_texels(self):
        origins, directions = checker_rays()
        for variant in ("scalar_rgb", "llvm_ad_rgb") if have_llvm() else ("scalar_rgb",):
            with self.subTest(variant=variant):
                query = self.query(variant)
                where, rays, faces = query.first_hit(origins, directions)
                np.testing.assert_array_equal(rays, np.arange(len(origins)))
                np.testing.assert_allclose(where[:, 2], np.where(SOLID, CARD_Z, PLANE_Z), atol=1e-6)
                np.testing.assert_array_equal(faces < 2, SOLID)
                hit, distance, triangles = query.first_hits(origins, directions)
                np.testing.assert_array_equal(hit, True)
                np.testing.assert_array_equal(triangles, faces)
                np.testing.assert_allclose(distance, EYE_Z - where[:, 2], atol=1e-6)
                _, doubled, _ = query.first_hits(origins, 2 * directions)
                np.testing.assert_allclose(doubled, distance / 2, atol=1e-6, err_msg="distance is not in direction lengths")
                np.testing.assert_array_equal(query.blocked(origins, directions), True)
                firsts = {}
                for tris, alive, _ in query.hit_rounds(origins, directions):
                    for tri, ray in zip(tris, alive):
                        firsts.setdefault(int(ray), int(tri))
                self.assertEqual(firsts, dict(enumerate(faces.tolist())))
                card = self.query(variant, plane=False)
                np.testing.assert_array_equal(card.blocked(origins, directions), SOLID)
                np.testing.assert_array_equal(card.first_hit(origins, directions)[1], np.flatnonzero(SOLID))


if __name__ == "__main__":
    unittest.main()
