"""Small alpha fixtures exercise the fit buffers and source-report consumers."""

import pathlib
import sys
import tempfile
import unittest
from contextlib import ExitStack
from types import SimpleNamespace as NS
from unittest.mock import patch

import numpy as np
from PIL import Image

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from r3d import ray_query, reference_render
from r3d.light import AlphaTest
from tests.test_r3d_alpha_reference import checker_source
from tests.test_r3d_alpha_cutout import checker_rays, KEEP_ALPHA, CARD_Z, PLANE_Z, EYE_Z

from r3d.fit.gbuffer import reference_buffers
from r3d.appearance_simplify import projection
from r3d.objects import instances_figure as figure
from r3d.objects import seams_report as seams
from r3d.objects import object_error as error
from r3d import fitted_variant, mesh_bundle, bake_fidelity
from r3d import appearance_simplify
from r3d.fit import copies
from tests.r3d_env import needs_llvm  # noqa: E402

try:
    import torch
except ImportError:
    torch = None


class AlphaConsumerTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.root = pathlib.Path(directory.name)
        self.source = checker_source(directory.name)
        self.source.alpha_test = AlphaTest(self.source, KEEP_ALPHA)
        self.variant = patch.object(ray_query, 'VARIANT', 'llvm_ad_rgb')
        self.variant.start()
        self.addCleanup(self.variant.stop)

    @unittest.skipIf(torch is None, "PyTorch is not installed")
    def test_reference_buffers_holes_hidden_and_opaque_raster(self):
        matrix = torch.tensor(projection([0., 0., EYE_Z], [0., 0., -1.], 4, 4, .4, .01))
        carved = self.source.tri_m == 0
        with patch('r3d.appearance_simplify.Renderer', side_effect=AssertionError('unexpected raster')):
            buffers = reference_buffers(self.source, carved, [matrix], [(4, 4)], 'cpu')
            self.assertTrue(buffers.copy[0].any())
            self.assertFalse(buffers.copy[0].all())
            expected = np.where(buffers.copy[0], EYE_Z-CARD_Z, EYE_Z-PLANE_Z)
            np.testing.assert_allclose(buffers.depth[0], expected, atol=1e-6)
            self.source.camera_hidden = carved
            hidden = reference_buffers(self.source, carved, [matrix], [(4, 4)], 'cpu')
            self.assertFalse(hidden.copy[0].any())
            np.testing.assert_allclose(hidden.depth[0], EYE_Z-PLANE_Z)
        self.source.camera_hidden = np.zeros(len(carved), bool)
        self.source.alpha_test = None
        with patch('r3d.appearance_simplify.Renderer') as renderer, \
                patch('r3d.fit.gbuffer.distance', return_value=(torch.ones((4, 4)), torch.zeros((4, 4), dtype=torch.long))):
            reference_buffers(self.source, carved, [matrix], [(4, 4)], 'cpu')
            renderer.assert_called_once()

    @needs_llvm
    def test_instances_figure_queries_through_alpha_holes(self):
        job = NS(settings=NS(alpha_keep=KEEP_ALPHA), object=NS(matrix=np.eye(3), position=np.zeros(3), name='mesh'))
        camera = NS(name='view', component=NS(path=NS(animation=None, node=None),
                                             half_fov_short_tan=.4, near_z=.01))
        calls = []
        def choose(boxes, owners, poses, size, lens, near, query):
            where, _, _ = query.first_hit(*checker_rays())
            np.testing.assert_allclose(where[:, 2], [CARD_Z, PLANE_Z, PLANE_Z, CARD_Z], atol=1e-6)
            calls.append(query.hit_filter)
            return 0, np.zeros((0, 2, 2)), [], {}, (0, 0, 0)
        with patch.object(figure, 'load_source', return_value=self.source), \
                patch.object(figure, 'sample_camera_path', return_value=(*figure.DOC_SIZE, .4, .01, [np.zeros(6)])), \
                patch.object(figure, 'choose_view', side_effect=choose):
            figure.generate(NS(path=pathlib.Path('fixture.scene.toml')), job, [camera], 'host', self.root/'figure.png',
                            1, host_render=lambda *args: Image.new('RGB', figure.DOC_SIZE))
        self.assertIsInstance(calls[0], AlphaTest)

    def test_seams_report_keeps_full_alpha_source_for_contacts(self):
        settings = NS(alpha_keep=KEEP_ALPHA, meshes=[dict(match=dict(material='card'))], position_scale=None,
                      variants=[NS(name='full')])
        job = NS(settings=settings, object=NS(name='mesh', kind='mesh_renderer'), renderer=NS(variant=NS(name='full')))
        geometry = NS(mesh_report={})
        with patch.object(seams, 'load_scene', return_value=NS(renderers=[job])), \
                patch.object(seams, 'load_import_settings', return_value=settings), \
                patch.object(seams, 'load_source', return_value=self.source), \
                patch.object(seams, 'bake_geometry', return_value=geometry), \
                patch.object(seams, 'contact_graph', return_value=[]) as contacts, \
                patch.object(seams, 'render_contacts'):
            out = self.root/'seams'
            self.assertEqual(seams.main(['fixture.scene.toml', '--object', 'mesh', '--settings', 'fixture.import.toml',
                                         '--host', 'host', '--out', str(out)]), 0)
        np.testing.assert_array_equal(contacts.call_args.args[1], self.source.tri_v)
        self.assertEqual(len(self.source.tri_v), 4)
        self.assertIsInstance(self.source.alpha_test, AlphaTest)

    @needs_llvm
    def test_held_out_error_assigns_parts_through_alpha_holes(self):
        source = self.source
        job = NS(settings=NS(alpha_keep=KEEP_ALPHA, double_sided=[]), asset_name='mesh', bake=None, object=NS(name='mesh'))
        mesh = NS(points=source.p, vertex_point=np.arange(len(source.p)), tris=source.tri_v, flat=False,
                  rgb=np.zeros((len(source.p), 3)))
        bundle = self.root/'mesh.bundle'
        bundle.write_bytes(b'')
        seen = []
        def accumulate(video, images, scale, ids_for):
            ids = ids_for(0, 4, 4)
            self.assertEqual(len(np.unique(ids)), 2)
            seen.append(ids)
            return None
        with ExitStack() as stack:
            started = stack.enter_context(patch.object(appearance_simplify, 'start_mesh', return_value=mesh))
            for target, value in ((reference_render, ('load_source', source)),
                                  (mesh_bundle, ('decode', ([('mesh', b'LMSH', b'')], []))),
                                  (copies, ('placed_copies', None)),
                                  (bake_fidelity, ('build_host', 'host')), (fitted_variant, ('held_out_score', (0., 0.))),
                                  (error, ('rows_for', ([], 0., 0.)))):
                stack.enter_context(patch.object(target, value[0], return_value=value[1]))
            stack.enter_context(patch('r3d.poses.read_poses', return_value=(4, 4, .4, .01,
                                        [np.array([0., 0., EYE_Z, 0., 0., -1.])])))
            stack.enter_context(patch.object(error, 'accumulate', side_effect=accumulate))
            error.held_out_error(pathlib.Path('fixture.scene.toml'), NS(), job, bundle, self.root, self.root)
        self.assertEqual(len(seen), 1)
        # start_mesh decodes a whole bundle, the rest beside its copies: never one entry's bytes.
        started.assert_called_once_with(bundle)
