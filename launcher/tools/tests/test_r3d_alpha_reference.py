"""Alpha-tested source hits, transport and reference buffers on layered checker cards."""

import pathlib
import sys
import tempfile
import unittest
from copy import copy
from unittest.mock import patch

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

try:
    import numpy as np
    from PIL import Image
    from r3d import light, reference_render
    from r3d.obj import Texture
    from r3d.ray_query import RayQuery
    from tests.test_r3d_mitsuba_reference import make_source, quad, mitsuba, bake_render, sun
except ImportError:
    np = None

from tests.r3d_env import needs_mitsuba
from tests.test_r3d_ray_query import have_llvm

CARD_Z = 1.0
PLANE_Z = 0.0
EYE_Z = 3.0
HALF = 1.0
KEEP_ALPHA = 0.5
TEXELS = 8
VARIANT = 'scalar_rgb'


def checker_source(directory, plane=True):
    y, x = np.indices((TEXELS, TEXELS))
    pixels = np.full((TEXELS, TEXELS, 4), 255, dtype=np.uint8)
    pixels[..., 3] = ((x // (TEXELS // 2) + y // (TEXELS // 2)) % 2) * 255
    path = pathlib.Path(directory) / 'checker.png'
    Image.fromarray(pixels).save(path)
    quads = [quad(CARD_Z, HALF)] + ([quad(PLANE_Z, HALF * 2)] if plane else [])
    with patch('tests.test_r3d_mitsuba_reference.soup.rays', return_value=None):
        source = make_source(quads, kd=[(1., 0., 0.), (0., 0., 1.)], texture=Texture(path),
                             materials=[0, 1] if plane else [0])
    source.tri_o = source.tri_m.copy()
    source.object_names = source.names
    return source


def checker_rays():
    uv = np.array([[0.2, 0.3], [0.7, 0.3], [0.2, 0.8], [0.7, 0.8]])
    xy = (uv * 2 - 1) * HALF
    origins = np.column_stack([xy, np.full(len(xy), EYE_Z)])
    directions = np.tile([0., 0., -1.], (len(xy), 1))
    return uv, origins, directions


@unittest.skipIf(np is None, 'the r3d environment is not installed')
class AlphaReferenceTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.source = checker_source(self.directory.name)

    def test_alpha_uses_the_hit_uv_and_only_samples_masked_materials(self):
        uv, origins, directions = checker_rays()
        locations = origins + directions * (EYE_Z - CARD_Z)
        # The first two positions fall below the diagonal; the other two above it.
        triangles = np.where(uv[:, 0] > uv[:, 1], 0, 1)
        opaque = copy(self.source.textures[0])
        opaque.levels = [np.ones_like(opaque.levels[0])]
        self.source.textures[1] = opaque
        alpha = light.AlphaTest(self.source, KEEP_ALPHA)
        np.testing.assert_array_equal(alpha(triangles, locations), [True, False, False, True])
        with patch.object(self.source.textures[0], 'sample', side_effect=AssertionError('masked texture sampled')), \
                patch.object(opaque, 'sample', side_effect=AssertionError('opaque texture sampled')):
            np.testing.assert_array_equal(alpha(np.array([2, 3]), locations[:2]), [True, True])
        np.testing.assert_array_equal(alpha(np.zeros(0, dtype=int), np.zeros((0, 3))), [])

    def test_threshold_includes_equality_and_texture_coordinates_repeat(self):
        alpha = light.AlphaTest(self.source, KEEP_ALPHA)
        np.testing.assert_array_equal(alpha.solid(np.array([0., KEEP_ALPHA, 1.])), [False, True, True])
        self.source.uv += [2., -1.]
        self.test_alpha_uses_the_hit_uv_and_only_samples_masked_materials()

    @needs_mitsuba
    def test_filtered_query_methods_agree_and_holes_reach_the_plane(self):
        for variant in (VARIANT, 'llvm_ad_rgb') if have_llvm() else (VARIANT,):
            with self.subTest(variant=variant):
                _, origins, directions = checker_rays()
                alpha = light.AlphaTest(self.source, KEEP_ALPHA)
                query = RayQuery(self.source.p, self.source.tri_v, variant, hit_filter=alpha)
                where, rays, faces = query.first_hit(origins, directions)
                np.testing.assert_array_equal(rays, np.arange(len(origins)))
                np.testing.assert_allclose(where[:, 2], [CARD_Z, PLANE_Z, PLANE_Z, CARD_Z], atol=1e-6)
                np.testing.assert_array_equal(faces < 2, [True, False, False, True])
                np.testing.assert_array_equal(query.blocked(origins, directions), True)
                hit, distance, triangles = query.first_hits(origins, directions)
                np.testing.assert_array_equal(hit, True)
                np.testing.assert_array_equal(triangles, faces)
                np.testing.assert_allclose(distance, EYE_Z - where[:, 2])
                active = np.ones(len(origins), bool)
                rounds = list(query.hit_rounds(origins, directions, active))
                for ray in range(len(origins)):
                    found = [(t, w) for ts, rs, ws in rounds for t, r, w in zip(ts, rs, ws) if r == ray]
                    self.assertEqual(found[0][0], faces[ray])
                    np.testing.assert_allclose(found[0][1], where[ray])
                source = checker_source(self.directory.name, plane=False)
                query = RayQuery(source.p, source.tri_v, variant, hit_filter=light.AlphaTest(source, KEEP_ALPHA))
                np.testing.assert_array_equal(query.blocked(origins, directions), [True, False, False, True])
                np.testing.assert_array_equal(query.first_hit(origins, directions)[1], [0, 3])

    @needs_mitsuba
    def test_drawn_hits_passes_transparent_texels(self):
        _, origins, directions = checker_rays()
        self.source.intersector = RayQuery(self.source.p, self.source.tri_v, VARIANT,
                                           hit_filter=light.AlphaTest(self.source, KEEP_ALPHA))
        where, rays, faces = reference_render.drawn_hits(self.source, [], origins, directions)
        np.testing.assert_array_equal(rays, np.arange(len(origins)))
        np.testing.assert_allclose(where[:, 2], [CARD_Z, PLANE_Z, PLANE_Z, CARD_Z], atol=1e-6)
        np.testing.assert_array_equal(faces < 2, [True, False, False, True])

    @needs_mitsuba
    def test_hidden_cards_pass_camera_rays_and_still_block_light(self):
        from types import SimpleNamespace as NS
        _, origins, directions = checker_rays()
        card = self.source.names[self.source.tri_m[0]]
        job = NS(settings=NS(alpha_keep=KEEP_ALPHA, cards=NS(material=card)))
        with patch.object(reference_render, 'load_source', return_value=self.source),                 patch.object(reference_render, 'path_light_for', return_value=None):
            source = reference_render.lit_source(job, NS(), variant=VARIANT, hide_cards=True)
        where, rays, _ = reference_render.drawn_hits(source, [], origins, directions)
        np.testing.assert_array_equal(rays, np.arange(len(origins)))
        np.testing.assert_allclose(where[:, 2], PLANE_Z, atol=1e-6)
        _, lit_rays, lit_faces = source.intersector.first_hit(origins, directions)
        np.testing.assert_array_equal(lit_rays, np.arange(len(origins)))
        np.testing.assert_array_equal(lit_faces < 2, [True, False, False, True])

    @needs_mitsuba
    def test_lit_source_keeps_triangles_and_attaches_the_same_filter(self):
        from types import SimpleNamespace as NS
        job = NS(settings=NS(alpha_keep=KEEP_ALPHA))
        with patch.object(reference_render, 'load_source', return_value=self.source), \
                patch.object(reference_render, 'path_light_for', return_value=None):
            source = reference_render.lit_source(job, NS(), variant=VARIANT)
        self.assertEqual(len(source.tri_v), 4)
        self.assertIs(source.intersector.hit_filter, source.alpha_test)

    @needs_mitsuba
    def test_mitsuba_mask_matches_primary_checker_under_direct_light(self):
        source = self.source
        source.alpha_test = light.AlphaTest(source, KEEP_ALPHA)
        source.intersector = RayQuery(source.p, source.tri_v, VARIANT, hit_filter=source.alpha_test)
        pose = np.array([0., 0., EYE_Z, 0., 0., -1.])
        lights = [sun([0., 0., 1.])]
        view = dict(pose=pose, width=24, height=24, lens=0.4)
        expected = bake_render(source, lights, **view)
        actual, coverage = mitsuba(source, lights, spp=32, depth=4, **view)
        centres = [(6, 6), (6, 18), (18, 6), (18, 18)]
        for y, x in centres:
            np.testing.assert_allclose(actual[y, x], expected[y, x], atol=0.03)
        self.assertGreater(expected[..., 0].max(), 0.9)
        self.assertGreater(expected[..., 2].max(), 0.9)
        np.testing.assert_allclose(coverage, 1.)

    @needs_mitsuba
    def test_reference_gbuffer_sees_the_plane_through_holes(self):
        from r3d.fit.gbuffer import traced_buffer
        from r3d.appearance_simplify import projection
        source = self.source
        source.alpha_test = light.AlphaTest(source, KEEP_ALPHA)
        query = RayQuery(source.p, source.tri_v, VARIANT, hit_filter=source.alpha_test)
        size = (4, 4)
        matrix = projection([0., 0., EYE_Z], [0., 0., -1.], *size, 0.4, 0.01)
        depth, faces = traced_buffer(query, matrix, size)
        expected_card = np.array([[True, True, False, False], [True, True, False, False],
                                  [False, False, True, True], [False, False, True, True]])
        np.testing.assert_array_equal(faces < 2, expected_card)
        np.testing.assert_allclose(depth, np.where(expected_card, EYE_Z - CARD_Z, EYE_Z - PLANE_Z))

    def test_bounce_cache_distinguishes_masked_reference_from_opaque_importer(self):
        from types import SimpleNamespace as NS
        from r3d import mesh_source
        mesh_source.PATH_LIGHTS.clear()
        self.addCleanup(mesh_source.PATH_LIGHTS.clear)
        job = NS(settings=NS(path='source', double_sided=[]), bake=NS(indirect=NS(bounces=1, rays=1)))
        scene = NS(indirect=NS(albedo_boost=1.), lights=[])
        with patch.object(mesh_source, 'PathLight', side_effect=[object(), object()]) as build:
            opaque = mesh_source.path_light_for(self.source, job, scene)
            self.source.alpha_test = light.AlphaTest(self.source, KEEP_ALPHA)
            masked = mesh_source.path_light_for(self.source, job, scene)
        self.assertIsNot(masked, opaque)
        self.assertEqual(build.call_count, 2)

    @needs_mitsuba
    def test_unfiltered_query_is_bit_identical_to_raw_trace_on_existing_soup(self):
        from tests.test_r3d_ray_query import SoupFixture
        SoupFixture.setUpClass()
        scene = SoupFixture
        for variant in (VARIANT, 'llvm_ad_rgb') if have_llvm() else (VARIANT,):
            with self.subTest(variant=variant):
                query = RayQuery(scene.positions, scene.tris, variant)
                raw = query._trace(scene.origins, scene.directions)
                actual = query.first_hits(scene.origins, scene.directions)
                np.testing.assert_array_equal(actual[0], raw[0])
                # Mitsuba leaves a miss's primitive index undefined.
                for observed, expected in zip(actual[1:], raw[1:]):
                    np.testing.assert_array_equal(observed[raw[0]], expected[raw[0]])
                np.testing.assert_array_equal(query.blocked(scene.origins, scene.directions), raw[0])
