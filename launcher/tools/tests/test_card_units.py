"""Unit contracts for card sampling, selection, page packing and camera filtering."""

from dataclasses import replace
import pathlib
import sys
import tempfile
import unittest
from types import SimpleNamespace as NS
from unittest.mock import patch

import numpy as np

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from asset.asset_pack import PackError
from r3d import lcol_asset, mesh_asset, mesh_bundle, mesh_source, ray_query, reference_render
from r3d.cards import (CARD_COVERED_MIN, CARD_MAX_SIDE, CARD_MASK_TEXELS, CARD_MIP_LEVELS,
                       CARD_SEE_THROUGH_MIN, decide, emit)
from r3d.cards.choose import card_from, probe_weights, split_cards, VISIBILITY_MARGIN
from r3d.cards.pages import pack_pages, PAGE_SIDE
from r3d.fit.copies import hidden_materials
from r3d.import_settings import load_geometry, SettingsError
from r3d.light import AlphaTest
from r3d.lit_mesh import bake_lit_mesh, build_lit_mesh, bundle_entries
from r3d.obj import Texture
from r3d.ray_query import RayQuery
from r3d.reference_render import camera_hidden, main
from r3d.texture import texture_asset as ltex, uv_transfer as luvs
from r3d.texture.texture_asset import mask_texture, MASK1_MIP_KEPT, MASK1_SIZE_LOG2_MIN, SIZE_LOG2_MAX
from r3d.views.probes import ProbeViews


class SharedOwnerTests(unittest.TestCase):
    def setUp(self):
        self.source = NS(p=np.eye(3), tri_v=np.array([[0, 1, 2]]), tri_t=np.array([[0, 1, 2]]),
                         tri_m=np.array([0]), names=['leaf'], textures=[None], uv=np.zeros((3, 2)))

    def test_rgba_samples_opaque_colour_too(self):
        texture = NS(levels=[np.ones((1, 1, 4))],
                     sample=lambda uv, lod: np.tile([0.2, 0.3, 0.4, 1.0], (len(uv), 1)))
        source = self.source
        source.textures = [texture]
        alpha = AlphaTest(source, 0.5)
        np.testing.assert_allclose(alpha.rgba(np.array([0]), np.array([[1/3] * 3])), [[0.2, 0.3, 0.4, 1]])
        np.testing.assert_array_equal(alpha(np.array([0]), np.array([[1/3] * 3])), [True])

    def test_card_decision_builds_one_source_query(self):
        source = self.source
        settings = NS(cards=NS(), alpha_keep=0.5, seed=0)
        views = NS(training=[], held_out=[], record={})
        def decide(src, options, make):
            return [make(0), make(0)], {}
        with patch.object(emit, 'RayQuery') as query, patch.object(emit, 'material_views', return_value=(views, None)), \
                patch.object(emit, 'make_shape', return_value=(None, {})), \
                patch('r3d.cards.decide.carded', side_effect=decide):
            emit.carded(source, settings, None)
        self.assertEqual(query.call_count, 1)

    def test_camera_filter_combines_hidden_and_alpha(self):
        source = NS(camera_hidden=np.array([True, False, False]),
                    alpha_test=NS(tested=np.array([True])))
        source.alpha_test = type('Alpha', (), {'tested': np.array([True]),
            '__call__': lambda self, hits, where: hits != 1})()
        keep = mesh_source.camera_filter(source)
        np.testing.assert_array_equal(keep(np.arange(3), np.zeros((3, 3))), [False, False, True])
        self.assertIsNone(mesh_source.camera_filter(NS()))


    def test_make_shape_reuses_samples_and_view_lens(self):
        source = self.source
        sampled = (np.eye(3), np.eye(3), np.ones(3))
        views = ProbeViews([], [], {}, face_lens=2.)
        with patch.object(emit, 'samples', return_value=sampled) as sample, \
                patch.object(emit, 'choose_cards', return_value=[]) as choose, patch.object(emit, 'RayQuery'):
            emit.make_shape(source, 0, NS(epsilon=1., area_cost=0.), 0, .5, views=views, query=None)
        sample.assert_called_once()
        self.assertIs(choose.call_args.args[0], sampled)
        self.assertIs(choose.call_args.args[1], sample.call_args.args[1])
        self.assertEqual(choose.call_args.kwargs['face_lens'], views.face_lens)


    def test_reference_sets_accept_materials_once(self):
        source = self.source
        job = NS(settings=NS(cards=NS(), alpha_keep=.5))
        inputs = NS(job=lambda: job, scene=lambda: NS())
        with tempfile.TemporaryDirectory() as folder, \
                patch.object(reference_render, 'load_source', return_value=source), \
                patch.object(reference_render, 'RayQuery'), patch.object(reference_render, 'path_light_for'), \
                patch.object(reference_render, 'read_poses', return_value=(1, 1, 1., .01, [])), \
                patch.object(reference_render, 'render_poses', return_value=(0, 1)) as render, \
                patch.object(emit, 'carded', return_value=([(0, {}, {})], {})) as accepted:
            reference_render.render_sets(inputs, [('one', pathlib.Path(folder)/'one'),
                                                   ('two', pathlib.Path(folder)/'two')])
        accepted.assert_called_once()
        self.assertEqual(render.call_count, 2)
        self.assertIs(render.call_args_list[0].args[0], render.call_args_list[1].args[0])
        np.testing.assert_array_equal(source.camera_hidden, [True])


class CardContractTests(unittest.TestCase):
    def test_rects_align_even_for_odd_masks_and_keep_coarsest_mip(self):
        padding = 1 << (CARD_MIP_LEVELS - 1)
        masks = [np.ones((padding + k, 2 * padding + k), bool) for k in (1, 2, 3)]
        pages, rects = pack_pages(masks)
        for mask, (page, x, y, width, height) in zip(masks, rects):
            self.assertEqual((x % padding, y % padding), (0, 0))
            local = np.pad(mask, ((0, -height % padding), (0, -width % padding)))
            for _ in range(CARD_MIP_LEVELS - 1):
                h, w = local.shape
                local = local.reshape(h // 2, 2, w // 2, 2).sum(axis=(1, 3)) >= MASK1_MIP_KEPT
            mip = np.asarray(mask_texture(pages[page], CARD_MIP_LEVELS).levels[-1])
            np.testing.assert_array_equal(mip[y//padding:y//padding+len(local),
                                               x//padding:x//padding+local.shape[1]], local)

    def test_page_refusals(self):
        for mask in (np.ones((0, 1), bool), np.ones((1, 0), bool), np.ones((PAGE_SIDE, 1), bool)):
            with self.subTest(shape=mask.shape), self.assertRaisesRegex(ValueError, 'does not fit'):
                pack_pages([mask])
        for levels in (0, SIZE_LOG2_MAX - MASK1_SIZE_LOG2_MIN + 2):
            with self.subTest(levels=levels), self.assertRaisesRegex(ValueError, 'mip count'):
                pack_pages([], levels)

    def test_second_mip_uses_children_coverage(self):
        bits = np.zeros((16, 16), bool)
        for count in range(5):
            y, x = divmod(count, 4)
            block = bits[y*4:y*4+4, x*4:x*4+4]
            children = block.reshape(2, 2, 2, 2).transpose(0, 2, 1, 3)
            for index in range(count):
                children[index//2, index%2] = True
        mip = np.asarray(mask_texture(bits, 3).levels[2])
        for count in range(5):
            self.assertEqual(bool(mip[divmod(count, 4)]), count >= MASK1_MIP_KEPT)

    def test_probe_weights_visibility_distance_lens_and_accumulation(self):
        points = np.array([[0., 0., 0.], [0., 0., -5.], [12., 0., 0.]])
        face = np.array([0., 0., 10., 0., 0., -1.])
        clear = NS(first_hits=lambda o, d: (np.zeros(len(o), bool), np.zeros(len(o)), np.zeros(len(o), int)))
        weights = probe_weights(points, [face], clear, 1.0)
        self.assertGreater(weights[0], 0)
        self.assertGreater(weights[0], weights[1])
        self.assertEqual(weights[2], 0)
        self.assertAlmostEqual(weights[weights > 0].mean(), 1)
        self.assertAlmostEqual(weights[0] / weights[1], (15/10)**2)
        self.assertGreater(probe_weights(points, [face], clear, 2.0)[2], 0)
        second = np.array([0., 0., -10., 0., 0., 1.])
        combined = probe_weights(points[:2], [face, second], clear, 1.0)
        self.assertLess(combined[0], combined[1])
        self.assertAlmostEqual(combined[0] / combined[1], (1/100 + 1/100) / (1/225 + 1/25))
        for z, visible in ((0.5, True), (2., False)):
            self.assertEqual(z < VISIBILITY_MARGIN, visible)
            wall = np.array([[-20., -20., z], [20., -20., z], [0., 20., z]])
            query = RayQuery(wall, np.array([[0, 1, 2]]), 'scalar_rgb')
            weight = probe_weights(points[:1], [face], query, 1.0)
            self.assertEqual(bool(weight[0]), visible)
        surface = RayQuery(np.array([[-20., -20., 0.], [20., -20., 0.], [0., 20., 0.]]),
                           np.array([[0, 1, 2]]), 'scalar_rgb')
        self.assertGreater(probe_weights(points[:1], [face], surface, 1.0)[0], 0)

    def test_split_caps_a_huge_card_among_small_cards(self):
        sizes = [CARD_MAX_SIDE / 2, CARD_MASK_TEXELS / 16, CARD_MASK_TEXELS / 16]
        cards = [dict(lo=np.zeros(2), hi=np.array([side, side])) for side in sizes]
        pieces, splits = split_cards(cards)
        self.assertGreater(splits, 0)
        masks = [np.ones(tuple(np.maximum(2, np.rint((c['hi'] - c['lo']) / c['texel_size']).astype(int))), bool)
                 for c in pieces]
        pages, rects = pack_pages(masks)
        self.assertEqual(len(rects), len(pieces))
        self.assertTrue(pages)

    def test_settings_epsilon_reaches_baked_slab(self):
        p = np.array([[-3., -3., -2.], [3., -3., -2.], [-3., 3., -2.], [3., 3., -2.]])
        t = np.array([[0, 1, 2], [2, 1, 3]])
        source = NS(p=p, tri_v=t, tri_t=t, tri_m=np.zeros(2, int), tri_o=np.zeros(2, int),
                    names=['leaf'], textures=[None], materials={}, uv=np.zeros((4, 2)))
        def plane(*args, epsilon, **kwargs):
            return [card_from(np.zeros(3), np.array([0., 0., 1.]), p * [1, 1, 0], epsilon)]
        views = ProbeViews([], [], {})
        with patch.object(emit, 'choose_cards', side_effect=plane), patch.object(ray_query, 'VARIANT', 'scalar_rgb'):
            for epsilon, kept in ((1., False), (3., True)):
                shape, report = emit.make_shape(source, 0, NS(epsilon=epsilon, area_cost=0.), 0, .5,
                                                views=views, query=None)
                self.assertEqual(shape is not None, kept)
                self.assertEqual(report['cards'] > 0, kept)

    def test_candidate_exclusions_and_inclusive_thresholds(self):
        source = NS(names=['leaf'], tri_m=np.array([0]), tri_o=np.array([0]), object_names=['leaf_object'],
                    textures=[Texture.from_levels([np.zeros((1, 1, 4))])])
        settings = NS(cards=NS(never=[], always=[]), meshes=[], thin=None, alpha_keep=.5)
        with patch.object(decide, 'see_through_share', return_value=CARD_SEE_THROUGH_MIN):
            self.assertEqual(decide.candidates(source, settings), [(0, CARD_SEE_THROUGH_MIN)])
            accepted, _ = decide.carded(source, settings, lambda m: ({}, {'covered': CARD_COVERED_MIN}))
            self.assertEqual(len(accepted), 1)
            settings.meshes = [dict(match=dict(material='leaf'))]
            self.assertEqual(decide.candidates(source, settings), [])
            settings.meshes = []
            settings.thin = NS(material='leaf')
            self.assertEqual(decide.candidates(source, settings), [])
        settings.thin = None
        settings.cards.always = ['leaf']
        accepted, report = decide.carded(source, settings, lambda m: (None, {'covered': 1.}))
        self.assertEqual(accepted, [])
        self.assertFalse(report['leaf']['accepted'])
        with patch.object(emit, 'carded', return_value=(accepted, report)):
            self.assertFalse(camera_hidden(source, settings).any())

    def test_card_settings_refuse_invalid_types_but_allow_zero_cost(self):
        base = dict(alpha_keep=None)
        geometry = dict(alpha_mask=dict(keep_alpha=.5), cards=dict(area_cost=0))
        settings = NS(**base)
        load_geometry(geometry, settings)
        self.assertEqual(settings.cards.area_cost, 0)
        for values in (dict(epsilon='bad'), dict(never='leaf'), dict(always='leaf'),
                       dict(never=[0]), dict(always=[0])):
            with self.subTest(values=values), self.assertRaises(SettingsError):
                load_geometry(dict(geometry, cards=values), NS(**base))
        settings = NS(**base)
        load_geometry({}, settings)
        self.assertIsNone(settings.cards)

    def test_mitsuba_hide_cards_is_a_parser_error(self):
        with self.assertRaises(SystemExit) as error, patch('sys.stderr'):
            main(['missing.scene.toml', '--poses', 'poses.txt', '--out', 'out', '--hide-cards', '--backend', 'mitsuba'])
        self.assertEqual(error.exception.code, 2)

class CardFormatTests(unittest.TestCase):
    def setUp(self):
        self.ltex, self.luvs = ltex, luvs
        self.mesh = NS(pos=np.zeros((3, 3)), tris=np.array([[0, 1, 2]]), clusters=[(0, 3, 0, 1)])
        self.uvs = luvs.pack_luvs(luvs.card_luvs(self.mesh, np.zeros((3, 2)), [0]))
        self.texture = ltex.mask_texture(np.ones((4, 4), bool), 1)

    def test_bundle_requires_mesh_and_texture_siblings(self):
        entries = [(self.luvs.name_for_mesh('card'), self.luvs.TYPE, self.uvs)]
        with self.assertRaisesRegex(PackError, 'LUVS sibling has no LMSH'):
            mesh_bundle.decode(mesh_bundle.encode(entries, []))
        entries.append(('card', mesh_asset.TYPE, b''))
        with self.assertRaisesRegex(PackError, 'material has no LTEX'):
            mesh_bundle.decode(mesh_bundle.encode(entries, []))

    def test_rename_rest_carries_uvs_and_all_pages(self):
        entries = [('old', mesh_asset.TYPE, b'mesh'), (self.luvs.name_for_mesh('old'), self.luvs.TYPE, self.uvs)]
        entries += [(self.luvs.texture_name('old', page), self.ltex.TYPE, bytes([page])) for page in range(3)]
        renamed = mesh_bundle.rename_rest(entries, [], 'new')
        expected = ['new', self.luvs.name_for_mesh('new')] + [self.luvs.texture_name('new', page) for page in range(3)]
        self.assertEqual([row[0] for row in renamed], expected)
        self.assertEqual([row[2] for row in renamed], [row[2] for row in entries])

    def test_mask1_pack_refusals(self):
        cases = [replace(self.texture, format=-1), replace(self.texture, palette=[]),
                 replace(self.texture, levels=[])]
        cases.append(replace(self.texture, levels=self.texture.levels * 2))
        cases += [replace(self.texture, levels=[np.full((4, 4), 2)]),
                  replace(self.texture, levels=[np.ones((4, 3), int)])]
        for value in cases:
            with self.subTest(value=value), self.assertRaises(ValueError):
                self.ltex.pack_ltex(value)

    def test_mask1_decode_refusals(self):
        entry = self.ltex.pack_ltex(self.texture)
        fields = list(self.ltex.HEADER.unpack_from(entry))
        cases = []
        for field, value in ((0, self.ltex.VERSION+1), (1, -1 & 0xffff),
                             (2, self.ltex.MASK1_SIZE_LOG2_MIN-1), (3, 0), (3, 2),
                             (5, self.ltex.HEADER.size+1), (6, self.ltex.HEADER.size)):
            changed = fields.copy()
            changed[field] = value
            cases.append(self.ltex.HEADER.pack(*changed) + entry[self.ltex.HEADER.size:])
        zero = fields.copy()
        zero[3] = 0
        cases.append(self.ltex.HEADER.pack(*zero) + entry[self.ltex.HEADER.size:fields[6]])
        cases.extend([entry[:-1], entry+b'\0'])
        for value in cases:
            with self.subTest(value=value), self.assertRaises(ValueError):
                self.ltex.decode(value)

    def test_card_uv_refusals(self):
        uv = np.zeros((3, 2))
        for coordinates, pages in ((uv[:-1], [0]), (uv, []), (uv, [-1]),
                                    (uv, [self.luvs.MATERIALS_MAX]),
                                    (uv + self.luvs.INT16.max + 1, [0]),
                                    (uv + self.luvs.INT16.min - 1, [0])):
            with self.subTest(pages=pages), self.assertRaises(ValueError):
                self.luvs.card_luvs(self.mesh, coordinates, pages)

    def test_card_triangle_cannot_span_pages(self):
        p = np.array([[0., 0., 0.], [2., 0., 0.], [0., 2., 0.]])
        with self.assertRaisesRegex(ValueError, 'a card triangle spans texture pages'):
            bake_lit_mesh(p, np.ones((3, 3)), np.array([[0, 1, 2]]), np.ones(1),
                          vertex_uvs=np.zeros((3, 2)), vertex_pages=np.array([0, 1, 0]))

    def test_fit_reads_hidden_materials_without_card_bake(self):
        entries = [('card', mesh_asset.TYPE, b''),
                   (self.luvs.name_for_mesh('card'), self.luvs.TYPE, self.uvs),
                   (self.luvs.texture_name('card', 0), self.ltex.TYPE, self.ltex.pack_ltex(self.texture))]
        row = dict(name='copy', mesh='card', colour_set=0, matrix=np.eye(3).tolist(), position=[0., 0., 0.],
                   owner='mesh', first='leaf_object', source='leaf_object', match=dict(material='leaf'))
        p = np.array([[0., 0., 0.], [2., 0., 0.], [0., 2., 0.]])
        mesh = build_lit_mesh(p, np.full((3, 3), 128), np.array([[0, 1, 2]]), np.ones(1), colourless=True)
        entries[0:1] = bundle_entries('card', mesh)
        entries += bundle_entries('rest', build_lit_mesh(p, np.full((3, 3), 128),
                                                       np.array([[0, 1, 2]]), np.ones(1)))
        entries.append((lcol_asset.name_for_mesh('card'), lcol_asset.TYPE,
                        lcol_asset.encode(lcol_asset.RGB888, len(mesh.pos), [bytes(len(mesh.pos)*3)])))
        with tempfile.TemporaryDirectory() as folder:
            bundle = pathlib.Path(folder) / 'start.bundle'
            bundle.write_bytes(mesh_bundle.encode(entries, [row]))
            with patch.object(emit, 'carded', side_effect=AssertionError('unexpected bake')):
                materials = hidden_materials(*mesh_bundle.decode(bundle.read_bytes()))
                self.assertEqual(materials, {'leaf'})
                from r3d.appearance_simplify import start_mesh
                with patch.object(pathlib.Path, 'read_bytes', autospec=True,
                                  side_effect=pathlib.Path.read_bytes) as read:
                    fitted = start_mesh(bundle)
                read.assert_called_once_with(bundle)
                self.assertEqual(fitted.hidden_materials, frozenset({'leaf'}))
                from r3d.fit import stage
                recipe = NS(steps=1, budget=1, batch=1, laplacian=0., normal_weight=0.,
                            surface_weight=1., surface_margin=0., plane_weight=0.)
                job = NS(asset_name='rest', object=NS(name='rest'),
                         settings=NS(fit=NS(probes=None, budget=None)),
                         renderer=NS(variant=NS(name='rest'), fit=recipe))
                def fit_main(command):
                    at = command.index('--hidden-materials')
                    self.assertEqual(command[at + 1], 'leaf')
                    rest = pathlib.Path(command[command.index('--start') + 1])
                    rest_entries, rest_rows = mesh_bundle.decode(rest.read_bytes())
                    self.assertEqual(hidden_materials(rest_entries, rest_rows), set())
                    raise InterruptedError('checked stage inputs')
                with patch('r3d.appearance_simplify.main', side_effect=fit_main), \
                        self.assertRaisesRegex(InterruptedError, 'checked stage inputs'):
                    stage.fit('scene', NS(camera=NS(component=NS(path=True))), job,
                              pathlib.Path(folder), start=bundle)
                source = NS(names=['leaf', 'wall'], tri_m=np.array([0, 1]))
                np.testing.assert_array_equal(camera_hidden(source, NS(), hidden_materials=materials), [True, False])
