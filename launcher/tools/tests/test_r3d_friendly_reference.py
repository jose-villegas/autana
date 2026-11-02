"""Checks the fitting-friendly references (r3d/fit/friendly_reference.py): the a-trous filter flattens texture
inside a surface and stops at part borders, creases and off-plane steps; the shader modes read each texture at
the affordable triangle's mip level or its mean. Needs NumPy."""

import pathlib
import sys
import unittest
from types import SimpleNamespace

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

try:
    import numpy as np
    from r3d.fit.footprint import affordable_area, mip_levels
    from r3d.fit.friendly_reference import ATROUS_LEVELS, PLANE_SIGMA, Guide, atrous, passes_for, shade
except ImportError:
    np = None

SIZE = 32


def floor_guide(passes=ATROUS_LEVELS):
    """A square of floor facing +y, one part, a world unit per pixel, every pixel taking `passes`."""
    y, x = np.mgrid[0:SIZE, 0:SIZE].astype(float)
    point = np.stack([x, np.zeros_like(x), y], axis=-1)
    normal = np.broadcast_to([0.0, 1.0, 0.0], point.shape).copy()
    return Guide(np.zeros((SIZE, SIZE), dtype=np.int64), point, normal, np.full((SIZE, SIZE), passes))


def checker():
    y, x = np.mgrid[0:SIZE, 0:SIZE]
    red = ((x + y) % 2).astype(float)
    return np.stack([red, np.zeros_like(red), np.zeros_like(red)], axis=-1)


@unittest.skipIf(np is None, "needs NumPy")
class AtrousTests(unittest.TestCase):
    EDGE = 1.0

    def left_bleed(self, guide):
        """The largest blue the left half takes from a blue right half."""
        colour = checker()
        colour[:, SIZE // 2:] = [0.0, 0.0, 1.0]
        return atrous(colour, guide, self.EDGE)[:, :SIZE // 2, 2].max()

    def test_texture_inside_a_surface_flattens_to_its_mean(self):
        filtered = atrous(checker(), floor_guide(), self.EDGE)
        inner = filtered[8:-8, 8:-8, 0]
        self.assertLess(np.abs(inner - 0.5).max(), 0.02)

    def test_part_borders_creases_and_steps_stop_it(self):
        border = floor_guide()
        border.part[:, SIZE // 2:] = 1
        crease = floor_guide()
        crease.normal[:, SIZE // 2:] = [1.0, 0.0, 0.0]
        step = floor_guide()
        # A step four plane widths up: exp(-16) per tap.
        step.point[:, SIZE // 2:, 1] = 4 * PLANE_SIGMA * self.EDGE
        for name, guide in (("border", border), ("crease", crease), ("step", step)):
            with self.subTest(name):
                self.assertLess(self.left_bleed(guide), 1e-5)
        self.assertGreater(self.left_bleed(floor_guide()), 0.1)

    def test_pixels_without_passes_or_hits_keep_their_colour(self):
        guide = floor_guide()
        guide.passes[:, :4] = 0
        guide.part[:4] = -1
        filtered = atrous(checker(), guide, self.EDGE)
        np.testing.assert_array_equal(filtered[:, :4], checker()[:, :4])
        np.testing.assert_array_equal(filtered[:4], checker()[:4])

    def test_passes_reach_half_the_affordable_triangle(self):
        # n passes reach 2 (2^n - 1) pixels; a triangle twice as wide as that needs exactly n.
        for n in range(1, ATROUS_LEVELS + 1):
            self.assertEqual(passes_for(np.array([4.0 * (2 ** n - 1)]))[0], n)
            self.assertEqual(passes_for(np.array([4.0 * (2 ** n - 1) + 1]))[0], min(n + 1, ATROUS_LEVELS))
        self.assertEqual(passes_for(np.array([0.0]))[0], 0)


class FakeTexture:
    def __init__(self, width, height):
        rng = np.random.default_rng(1)
        level = rng.random((height, width, 4))
        self.levels = [level]
        while min(level.shape[:2]) > 1:
            level = level[::2, ::2]
            self.levels.append(level)

    @property
    def size(self):
        return self.levels[0].shape[1], self.levels[0].shape[0]


@unittest.skipIf(np is None, "needs NumPy")
class ShaderTests(unittest.TestCase):
    TEXELS = 64
    # The texture's texel is this many world units wide on the quad.
    TEXEL = 0.25

    def source(self):
        side = self.TEXELS * self.TEXEL
        p = np.array([[0, 0, 0], [side, 0, 0], [side, 0, side], [0, 0, side]], dtype=float)
        uv = np.array([[0, 0], [1, 0], [1, 1], [0, 1]], dtype=float)
        tri = np.array([[0, 1, 2], [0, 2, 3]])
        return SimpleNamespace(p=p, uv=uv, tri_v=tri, tri_t=tri, tri_m=np.array([0, 0]),
                               textures=[FakeTexture(self.TEXELS, self.TEXELS), None], names=["stone", "paint"])

    def test_the_mip_texel_spans_the_affordable_triangle(self):
        source = self.source()
        for level in range(4):
            edge = self.TEXEL * 2 ** level
            self.assertEqual(mip_levels(source, edge ** 2), {0: level})
        # A budget finer than the texture reads level 0; one coarser than the whole texture its last level.
        self.assertEqual(mip_levels(source, (self.TEXEL / 4) ** 2), {0: 0})
        self.assertEqual(mip_levels(source, (self.TEXEL * 2 ** 12) ** 2), {0: len(source.textures[0].levels) - 1})

    def test_affordable_area_shares_the_surface_by_the_budget(self):
        source = self.source()
        self.assertAlmostEqual(affordable_area(source.p, source.tri_v, 8), (self.TEXELS * self.TEXEL) ** 2 / 8)

    def test_shading_reads_from_the_budget_level_or_the_mean(self):
        source = self.source()
        levels = list(source.textures[0].levels)
        shade(source, "budget", (self.TEXEL * 4) ** 2)
        self.assertIs(source.textures[0].levels[0], levels[2])
        source = self.source()
        mean = source.textures[0].levels[0].mean(axis=(0, 1))
        shade(source, "average", 1.0)
        [level] = source.textures[0].levels
        np.testing.assert_allclose(level.reshape(-1), mean)


if __name__ == "__main__":
    unittest.main()
