"""Alpha-tested source hits, transport and reference buffers on layered checker cards."""

import pathlib
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

try:
    import numpy as np
    from r3d import light, reference_render
    from r3d.ray_query import RayQuery
    from tests.test_r3d_mitsuba_reference import mitsuba, bake_render, sun
    from tests.test_r3d_alpha_cutout import (checker_source as cutout_source, checker_rays,
                                             CARD_Z, PLANE_Z, EYE_Z, KEEP_ALPHA)
except ImportError:
    np = None

from tests.r3d_env import needs_mitsuba
from tests.test_r3d_ray_query import have_llvm

VARIANT = 'scalar_rgb'


def checker_source(directory):
    source = cutout_source(directory)
    source.names = ['card', 'plane']
    source.materials = {'card': {'Kd': (1., 0., 0.)}, 'plane': {'Kd': (0., 0., 1.)}}
    source.colors = None
    source.tri_o = source.tri_m.copy()
    source.object_names = source.names
    from r3d.geometry import corner_normals
    source.corner_normals = corner_normals(source.p, source.tri_v)
    source.twin = light.coincident_faces(source.p, source.tri_v)[0]
    source.bounce = None
    return source


@unittest.skipIf(np is None, 'the r3d environment is not installed')
class AlphaReferenceTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.source = checker_source(self.directory.name)

    @needs_mitsuba
    def test_drawn_hits_passes_transparent_texels(self):
        origins, directions = checker_rays()
        self.source.intersector = RayQuery(self.source.p, self.source.tri_v, VARIANT,
                                           hit_filter=light.AlphaTest(self.source, KEEP_ALPHA))
        where, rays, faces = reference_render.drawn_hits(self.source, [], origins, directions)
        np.testing.assert_array_equal(rays, np.arange(len(origins)))
        np.testing.assert_allclose(where[:, 2], [CARD_Z, PLANE_Z, PLANE_Z, CARD_Z], atol=1e-6)
        np.testing.assert_array_equal(faces < 2, [True, False, False, True])

    @needs_mitsuba
    def test_drawn_hits_samples_alpha_once_per_query_hit(self):
        origins, directions = checker_rays()
        self.source.alpha_test = light.AlphaTest(self.source, KEEP_ALPHA)
        self.source.intersector = RayQuery(self.source.p, self.source.tri_v, VARIANT,
                                           hit_filter=self.source.alpha_test)
        texture = self.source.textures[0]
        with patch.object(texture, 'sample', wraps=texture.sample) as sample:
            reference_render.drawn_hits(self.source, [], origins, directions)
        self.assertEqual(sum(len(call.args[0]) for call in sample.call_args_list), len(origins))

    @needs_mitsuba
    def test_mitsuba_thresholds_bilinear_alpha_after_sampling(self):
        from r3d import mitsuba_reference
        from r3d.obj import Texture
        mi = mitsuba_reference.import_mitsuba()
        for variant in ('scalar_rgb', 'llvm_ad_rgb') if have_llvm() else ('scalar_rgb',):
            mi.set_variant(variant)
            level = np.ones((2, 2, 4))
            level[:, 0, 3] = 0.
            texture = Texture.from_levels([level])
            self.source.textures[0] = texture
            alpha = light.AlphaTest(self.source, .75)
            bsdf = mi.load_dict(mitsuba_reference.material_bsdf(mi, (1., 1., 1.), texture, False,
                                                               alpha_test=alpha))
            for u in (.55, .625, .65):
                si = mi.SurfaceInteraction3f()
                si.uv = [u, .5]
                actual = 1. - float(np.asarray(bsdf.eval_null_transmission(si)).ravel()[0])
                location = np.array([[u * 2 - 1, 0., CARD_Z]])
                expected = alpha(np.array([0]), location)[0]
                self.assertEqual(actual, float(expected), (variant, u))

    @needs_mitsuba
    def test_hidden_cards_pass_camera_rays_and_still_block_light(self):
        from types import SimpleNamespace as NS
        origins, directions = checker_rays()
        card = self.source.names[self.source.tri_m[0]]
        job = NS(settings=NS(alpha_keep=KEEP_ALPHA, seed=0, thin=None, meshes=[],
                             cards=NS(never=[], always=[card],
                                      epsilon=CARD_Z, area_cost=0.0)))
        with patch.object(reference_render, 'load_source', return_value=self.source),                 patch.object(reference_render, 'path_light_for', return_value=None), \
                patch('r3d.cards.emit.material_views', return_value=(None, None)), \
                patch('r3d.cards.emit.make_shape', return_value=({}, {'covered': 1.0, 'covered_area': 1.0})):
            source = reference_render.lit_source(job, NS(), hide_cards=True)
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
            source = reference_render.lit_source(job, NS(), )
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
