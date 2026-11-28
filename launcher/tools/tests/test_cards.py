"""Synthetic billboard selection, alpha baking and MASK1/LUVS emission."""
import pathlib
import sys
import tempfile
import unittest
from types import SimpleNamespace
from unittest.mock import patch

import numpy as np

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from r3d.cards import CARD_EPSILON, CANDIDATE_MIN_SAMPLES, SAMPLES_PER_TRIANGLE
from r3d.cards.choose import card_from, choose_cards as select_cards, probe_weights, samples, split_cards
from r3d.views.probes import FACE_LENS
from r3d.cards.bake import bake_card, corner_colours
from r3d import mesh_bundle, mesh_import
from r3d.lit_mesh import read_bundle_mesh
from r3d.texture import texture_asset, uv_transfer
from r3d.cards import CARD_MASK_TEXELS
from r3d.obj import Texture
from r3d.mesh_source import with_triangles
from r3d.light import AlphaTest
from r3d.ray_query import RayQuery
from tests import soup  # noqa: F401
from tests.r3d_env import needs_llvm  # noqa: E402

SIDE = CARD_EPSILON * 12
SEEDS = range(3)
SUBDIVISIONS = int(np.ceil(np.sqrt(CANDIDATE_MIN_SAMPLES)))
NORMAL_TOLERANCE = 0.02
CHECKER_TEXELS = CARD_MASK_TEXELS
CHECKER_BLOCK = CHECKER_TEXELS // 2


def quads(crossed=False):
    x, y = np.meshgrid(np.linspace(-SIDE / 2, SIDE / 2, SUBDIVISIONS + 1),
                       np.linspace(-SIDE / 2, SIDE / 2, SUBDIVISIONS + 1))
    p = np.column_stack([x.ravel(), y.ravel(), np.zeros(x.size)])
    ids = np.arange(len(p)).reshape(x.shape)
    low = ids[:-1, :-1].ravel()
    right, up = low + 1, low + SUBDIVISIONS + 1
    t = np.concatenate([np.column_stack([low, right, up]), np.column_stack([up, right, up + 1])])
    if crossed:
        t = np.concatenate([t, t + len(p)])
        p = np.concatenate([p, p[:, [2, 1, 0]]])
    return p, t


def choose_cards(p, t, **options):
    from r3d.ray_query import RayQuery
    from r3d.views.probes import cube_poses
    views = options.pop('views', cube_poses([[0, 0, SIDE], [SIDE, 0, 0], [-SIDE, 0, 0]]))
    rng = np.random.default_rng(options.pop('seed', 0))
    sub = with_triangles(SimpleNamespace(p=p, textures=[options.pop('texture', None)]), t,
                         options.pop('tri_uv', t), np.zeros(len(t), int),
                         uv=options.pop('uv', p[:, :2] / SIDE + 0.5))
    sampled = samples(sub, rng, options.pop('keep_alpha', 0.5))
    weights = probe_weights(sampled[0], views, RayQuery(p, t), FACE_LENS, near_floor=0.)
    return split_cards(select_cards(sampled, rng, weights=weights, **options))[0]


def card_settings(path):
    from r3d.import_settings import load_import_settings
    settings = load_import_settings(path)
    settings.fit.probes = SimpleNamespace(spacing=SIDE / 2, face=None)
    return settings


def leaf_import(root, p, t):
    """A checkered leaf quad grid replaced by cards and one solid triangle above it, as an import in `root`."""
    from PIL import Image
    checker = np.zeros((CHECKER_TEXELS, CHECKER_TEXELS, 4), dtype=np.uint8)
    checker[..., :3] = [64, 128, 192]
    checker[..., 3] = ((np.indices(checker.shape[:2]) // CHECKER_BLOCK).sum(axis=0) % 2) * 255
    Image.fromarray(checker).save(root / 'leaf.png')
    (root / 'source.mtl').write_text('newmtl leaf\nKd 1 1 1\nmap_Kd leaf.png\nnewmtl solid\nKd 1 1 1\n')
    lines = ['mtllib source.mtl'] + ['v ' + ' '.join(map(str, row)) for row in p]
    lines += ['vt ' + ' '.join(map(str, row)) for row in (p[:, :2] / SIDE + 0.5)]
    lines += ['o leaf_object', 'usemtl leaf']
    lines += ['f ' + ' '.join(f'{i + 1}/{i + 1}' for i in tri) for tri in t]
    lines += ['o solid_object', 'usemtl solid', f'v 0 0 {SIDE}', f'v {SIDE} 0 {SIDE}',
              f'v 0 {SIDE} {SIDE}', 'f ' + ' '.join(str(len(p) + i) for i in (1, 2, 3))]
    (root / 'source.obj').write_text('\n'.join(lines))
    settings_path = root / 'mesh.import.toml'
    settings_path.write_text('[source]\npath = "source.obj"\ncredit = "synthetic"\n'
        '[output]\nname = "mesh"\n[geometry]\nalpha_mask = { keep_alpha = 0.5 }\n')
    return settings_path


class CardTests(unittest.TestCase):
    def import_fixture(self):
        from r3d.import_settings import load_import_settings
        from r3d.mesh_source import load_source
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        settings = card_settings(leaf_import(pathlib.Path(directory.name), *quads()))
        return settings, load_source(settings)

    @needs_llvm
    def test_alpha_mask_alone_cards_leaf(self):
        from r3d.import_settings import albedo_jobs
        settings, src = self.import_fixture()
        for seed in SEEDS:
            settings.seed = seed
            geometry = mesh_import.bake_geometry(albedo_jobs(settings)[0], None, carve=True)
            report = geometry.mesh_report['cards']
            self.assertEqual(set(report), {'leaf'})
            self.assertTrue(report['leaf']['accepted'])
            self.assertEqual([shape['name'] for shape in geometry.shapes], ['leaf'])
            self.assertNotIn(src.names.index('leaf'), geometry.tri_mat)
        leaf = src.tri_m == src.names.index('leaf')
        other = len(src.names)
        src.names.append('other_leaf')
        src.textures.append(src.textures[src.names.index('leaf')])
        src.tri_v = np.concatenate([src.tri_v, src.tri_v[leaf] + len(src.p)])
        src.tri_t = np.concatenate([src.tri_t, src.tri_t[leaf] + len(src.uv)])
        src.p = np.concatenate([src.p, src.p + [SIDE * 2, 0, 0]])
        src.uv = np.concatenate([src.uv, src.uv])
        src.tri_m = np.concatenate([src.tri_m, np.full(leaf.sum(), other)])
        src.tri_o = np.concatenate([src.tri_o, src.tri_o[leaf]])
        with patch.object(mesh_import, 'load_source', return_value=src):
            geometry = mesh_import.bake_geometry(albedo_jobs(settings)[0], None, carve=True)
        self.assertEqual(set(geometry.mesh_report['cards']), {'leaf', 'other_leaf'})
        self.assertEqual({shape['name'] for shape in geometry.shapes}, {'leaf', 'other_leaf'})
        self.assertEqual(geometry.mesh_report['scene_total'],
                         len(geometry.tris) + sum(report['triangles'] for report in geometry.mesh_report['cards'].values()))

    @needs_llvm
    def test_fringe_box_is_not_a_candidate(self):
        from r3d.cards.decide import candidates
        settings, src = self.import_fixture()
        box = soup.box([SIDE] * 3)
        src.p, src.tri_v = box.vertices, box.faces
        src.tri_t = np.tile(np.arange(3), (len(src.tri_v), 1))
        src.uv = np.array([[0.25, 0.25], [0.75, 0.25], [0.5, 0.75]])
        src.tri_m = np.full(len(src.tri_v), src.names.index('leaf'))
        src.tri_o = np.zeros(len(src.tri_v), int)
        texture = src.textures[src.names.index('leaf')]
        pixels = np.ones_like(texture.levels[0])
        pixels[[0, -1], :, 3] = 0
        pixels[:, [0, -1], 3] = 0
        texture.levels = [pixels]
        self.assertEqual(candidates(src, settings), [])

    @needs_llvm
    def test_low_coverage_stays_geometry(self):
        from r3d.cards import CARD_COVERED_MIN, decide
        from r3d.import_settings import albedo_jobs
        settings, _ = self.import_fixture()
        with patch.object(decide, 'CARD_COVERED_MIN', 1 + CARD_COVERED_MIN):
            geometry = mesh_import.bake_geometry(albedo_jobs(settings)[0], None, carve=True)
        self.assertFalse(geometry.mesh_report['cards']['leaf']['accepted'])
        self.assertFalse(geometry.shapes)
        self.assertIn(geometry.src.names.index('leaf'), geometry.tri_mat)

    @needs_llvm
    def test_all_geometry_replaced_by_cards_with_simplification(self):
        from r3d.import_settings import albedo_jobs
        settings, src = self.import_fixture()
        leaf = src.tri_m == src.names.index('leaf')
        from r3d.mesh_source import with_triangles
        src = with_triangles(src, src.tri_v[leaf], src.tri_t[leaf], src.tri_m[leaf], tri_o=src.tri_o[leaf])
        settings.simplify = SimpleNamespace(dense_edge=SIDE, props=[], props_share=0.,
                                           colour_deviation=1., seal_seams=False)
        job = albedo_jobs(settings)[0]
        job.renderer.variant.triangles = len(src.tri_v)
        with patch.object(mesh_import, 'load_source', return_value=src):
            geometry = mesh_import.bake_geometry(job, None, carve=True)
        self.assertEqual(geometry.tris.shape, (0, 3))
        self.assertTrue(geometry.shapes)
        self.assertTrue(geometry.mesh_report['cards']['leaf']['accepted'])

    @needs_llvm
    def test_never_keeps_passing_leaf(self):
        from r3d.import_settings import albedo_jobs
        settings, _ = self.import_fixture()
        settings.cards.never = ['leaf']
        geometry = mesh_import.bake_geometry(albedo_jobs(settings)[0], None, carve=True)
        self.assertEqual(geometry.mesh_report['cards'], {})
        self.assertFalse(geometry.shapes)
        self.assertIn(geometry.src.names.index('leaf'), geometry.tri_mat)

    @needs_llvm
    def test_always_overrides_failed_coverage(self):
        from r3d.cards import CARD_COVERED_MIN, decide
        from r3d.import_settings import albedo_jobs
        settings, _ = self.import_fixture()
        settings.cards.always = ['leaf']
        with patch.object(decide, 'CARD_COVERED_MIN', 1 + CARD_COVERED_MIN):
            geometry = mesh_import.bake_geometry(albedo_jobs(settings)[0], None, carve=True)
        self.assertTrue(geometry.mesh_report['cards']['leaf']['accepted'])
        self.assertTrue(geometry.mesh_report['cards']['leaf']['forced'])

    def test_unknown_override_is_an_error(self):
        from r3d.cards.decide import candidates
        from r3d.import_settings import SettingsError
        settings, src = self.import_fixture()
        for key in ('never', 'always'):
            setattr(settings.cards, key, ['absent'])
            with self.assertRaisesRegex(SettingsError, 'source lacks'):
                candidates(src, settings)
            setattr(settings.cards, key, [])

    def test_always_carved_is_an_error_but_always_thinned_is_not(self):
        from r3d.cards.decide import candidates
        from r3d.import_settings import SettingsError
        settings, src = self.import_fixture()
        settings.cards.always = ['leaf']
        settings.meshes = [dict(match=dict(material='leaf'))]
        with self.assertRaisesRegex(SettingsError, 'carved by an objects.meshes rule'):
            candidates(src, settings)
        settings.meshes = []
        settings.thin = SimpleNamespace(material='leaf', keep=.5)
        self.assertEqual(candidates(src, settings), [(src.names.index('leaf'), None)])

    @needs_llvm
    def test_cards_override_the_thin_rule(self):
        """A thinned material is a card candidate: accepted, none of its faces reach the baked geometry; rejected,
        it bakes exactly as the thin rule alone bakes it, which differs from no rule at all."""
        from r3d.cards import CARD_COVERED_MIN, decide
        from r3d.import_settings import albedo_jobs
        settings, src = self.import_fixture()
        leaf = src.names.index('leaf')

        def leaf_corners(floor=CARD_COVERED_MIN, thin=True, never=()):
            settings.thin = SimpleNamespace(material='leaf', keep=.25) if thin else None
            settings.cards.never = list(never)
            with patch.object(decide, 'CARD_COVERED_MIN', floor):
                geometry = mesh_import.bake_geometry(albedo_jobs(settings)[0], None, carve=True)
            corners = geometry.positions[geometry.tris[geometry.tri_mat == leaf]].reshape(-1, 3)
            return geometry, np.round(corners[np.lexsort(corners.T)], 6)

        geometry, corners = leaf_corners()
        self.assertTrue(geometry.mesh_report['cards']['leaf']['accepted'])
        self.assertEqual([shape['name'] for shape in geometry.shapes], ['leaf'])
        self.assertEqual(len(corners), 0, "an accepted material leaves no face for the thin rule")
        geometry, rejected = leaf_corners(floor=1 + CARD_COVERED_MIN)
        self.assertFalse(geometry.mesh_report['cards']['leaf']['accepted'])
        self.assertFalse(geometry.shapes)
        np.testing.assert_array_equal(rejected, leaf_corners(never=['leaf'])[1])
        unthinned = leaf_corners(floor=1 + CARD_COVERED_MIN, thin=False)[1]
        self.assertFalse(rejected.shape == unthinned.shape and np.array_equal(rejected, unthinned),
                         "a rejected material is thinned")

    def test_a_carved_and_thinned_material_is_not_a_candidate(self):
        from r3d.cards.decide import candidates
        settings, src = self.import_fixture()
        settings.thin = SimpleNamespace(material='leaf', keep=.5)
        settings.meshes = [dict(match=dict(material='leaf'))]
        self.assertEqual(candidates(src, settings), [])

    def test_sponza_leaves_are_thinned_but_carved_by_no_rule(self):
        from r3d.import_settings import load_import_settings
        settings = load_import_settings(pathlib.Path(__file__).resolve().parents[2] / 'demo/sponza/sponza.import.toml')
        self.assertEqual((settings.thin.material, settings.thin.keep), ('leaf', .35))
        self.assertFalse([rule for rule in settings.meshes if rule['match'].get('material') == 'leaf'])

    @needs_llvm
    def test_camera_hidden_is_exactly_accepted_materials(self):
        from r3d.cards import CARD_COVERED_MIN, decide
        from r3d.reference_render import camera_hidden
        settings, src = self.import_fixture()
        leaf = src.tri_m == src.names.index('leaf')
        np.testing.assert_array_equal(camera_hidden(src, settings), leaf)
        from r3d import reference_render
        with patch.object(reference_render, 'load_source', return_value=src) as load:
            source = reference_render.lit_source(SimpleNamespace(settings=settings), None, lit=False, hide_cards=True)
        load.assert_called_once_with(settings, np.float64)
        np.testing.assert_array_equal(source.camera_hidden, leaf)
        with patch.object(decide, 'CARD_COVERED_MIN', 1 + CARD_COVERED_MIN):
            self.assertFalse(camera_hidden(src, settings).any())
        settings.cards.never = ['leaf']
        self.assertFalse(camera_hidden(src, settings).any())

    @needs_llvm
    def test_one_flat_quad_and_crossed_quads(self):
        for seed in SEEDS:
            for crossed in (False, True):
                with self.subTest(seed=seed, crossed=crossed):
                    p, t = quads(crossed)
                    cards = choose_cards(p, t, seed=seed)
                    self.assertEqual(len(cards), 2 if crossed else 1)
                    normals = np.abs(np.array([c['n'] for c in cards]))
                    self.assertTrue(np.all(normals.max(axis=1) > 1 - NORMAL_TOLERANCE))
                    if crossed:
                        self.assertEqual(set(normals.argmax(axis=1)), {0, 2})

    @needs_llvm
    def test_samples_drop_transparent_uvs_and_keep_triangle_weights(self):
        from r3d.cards.choose import samples
        p, t = quads(True)
        uv = p[:, :2] / SIDE + 0.5
        # Opaque where u >= 0.5: the bilinear alpha crosses 0.5 exactly there.
        level = np.ones((CHECKER_TEXELS, CHECKER_TEXELS, 4))
        level[:, :CHECKER_TEXELS // 2, 3] = 0.0
        texture = SimpleNamespace(levels=[level])
        for seed in SEEDS:
            sub = with_triangles(SimpleNamespace(p=p, uv=uv, textures=[texture]), t, t, np.zeros(len(t), int))
            points, normals, area = samples(sub, np.random.default_rng(seed), 0.5)
            self.assertGreater(len(points), CANDIDATE_MIN_SAMPLES)
            self.assertLess(len(points), len(t) * SAMPLES_PER_TRIANGLE)
            self.assertTrue(np.all(points[:, 0] >= 0))
            np.testing.assert_allclose(np.linalg.norm(normals, axis=1), 1)
            triangle_area = (SIDE / SUBDIVISIONS) ** 2 / 2
            np.testing.assert_allclose(area, triangle_area / SAMPLES_PER_TRIANGLE)
            clear = SimpleNamespace(levels=[np.zeros_like(level)])
            self.assertEqual(choose_cards(p, t, uv=uv, tri_uv=t, texture=clear, keep_alpha=0.5, seed=seed), [])

    @needs_llvm
    def test_area_cost_can_stop_selection(self):
        p, t = quads(True)
        for seed in SEEDS:
            self.assertGreater(len(choose_cards(p, t, area_cost=0, seed=seed)), 0)
            self.assertEqual(choose_cards(p, t, area_cost=100, seed=seed), [])

    @needs_llvm
    def test_probe_weights_unseen_quad_to_zero(self):
        p, t = quads()
        p = np.concatenate([p, p + [SIDE * 4, 0, 0]])
        t = np.concatenate([t, t + len(p) // 2])
        poses = [np.array([0, 0, SIDE, 0, 0, -1])]
        for seed in SEEDS:
            cards = choose_cards(p, t, seed=seed, views=poses)
            self.assertEqual(len(cards), 1)
            self.assertLess(abs(cards[0]['c'][0]), SIDE)

    @needs_llvm
    def test_coverage_counts_what_the_probes_see(self):
        from r3d.cards import CARD_COVERED_MIN
        from r3d.cards.emit import make_shape
        from r3d.mesh_source import load_source
        from r3d.views.probes import FACE_LENS
        p, t = quads()
        # A second grid of the same leaf far beside the first, which no probe sees.
        p = np.concatenate([p, p + [SIDE * 4, 0, 0]])
        t = np.concatenate([t, t + len(p) // 2])
        with tempfile.TemporaryDirectory() as directory:
            settings = card_settings(leaf_import(pathlib.Path(directory), p, t))
            src = load_source(settings)
            leaf = src.names.index('leaf')
            views = SimpleNamespace(training=[np.array([0, 0, SIDE, 0, 0, -1])], held_out=[], face_lens=FACE_LENS,
                                    record={'spacing': 0.})
            query = RayQuery(src.p, src.tri_v, hit_filter=AlphaTest(src, settings.alpha_keep))
            _, report = make_shape(src, leaf, settings.cards, settings.seed, settings.alpha_keep, views=views,
                                   query=query)
        self.assertGreaterEqual(report['covered'], CARD_COVERED_MIN)
        self.assertLess(report['covered_area'], CARD_COVERED_MIN)

    @needs_llvm
    def test_split_masks_keep_parent_texel_size(self):
        from unittest.mock import Mock
        from r3d.cards import CARD_MAX_SIDE
        from r3d.cards.bake import bake_pixels
        p, t = quads()
        card = choose_cards(p, t)[0]
        card['lo'] = np.zeros(2)
        card['hi'] = np.array([CARD_MAX_SIDE * 4, CARD_MAX_SIDE * 2])
        pieces, _ = split_cards([card])
        query = Mock()
        query.hit_rounds.return_value = []
        card = dict(card, texel_size=pieces[0]["texel_size"])
        parent, _, _ = bake_pixels(card, query)
        masks = [bake_pixels(piece, query)[0] for piece in pieces]
        self.assertEqual(parent.shape, (CARD_MASK_TEXELS // 2, CARD_MASK_TEXELS))
        self.assertEqual(sum(mask.size for mask in masks), parent.size)
        self.assertTrue(all(mask.shape == (CARD_MASK_TEXELS // 4,) * 2 for mask in masks))
        for piece in pieces:
            trimmed = dict(piece, hi=piece['lo'] + (piece['hi'] - piece['lo']) / 2)
            mask = bake_pixels(trimmed, query)[0]
            self.assertEqual(mask.shape, (CARD_MASK_TEXELS // 8,) * 2)

    @needs_llvm
    def test_trimmed_mask_keeps_parent_texel_size(self):
        p, t = quads()
        uv = p[:, :2] / SIDE + 0.5
        card = dict(c=np.zeros(3), a=np.eye(3)[0], b=np.eye(3)[1], n=np.eye(3)[2],
                    lo=np.full(2, -SIDE / 2), hi=np.full(2, SIDE / 2), depth=(-CARD_EPSILON, CARD_EPSILON),
                    texel_size=SIDE / CARD_MASK_TEXELS)
        # Opaque where both texture coordinates lie in 0.25..0.75.
        side = CHECKER_TEXELS
        level = np.ones((side, side, 4))
        level[..., 3] = 0.0
        level[side // 4:3 * side // 4, side // 4:3 * side // 4, 3] = 1.0
        texture = SimpleNamespace(levels=[level])
        sub = with_triangles(SimpleNamespace(p=p, uv=uv, textures=[texture]), t, t, np.zeros(len(t), int))
        query = RayQuery(p, t, hit_filter=AlphaTest(sub, 0.5))
        trimmed, mask, _, _ = bake_card(card, query)
        self.assertEqual(mask.shape, (CARD_MASK_TEXELS // 2,) * 2)
        np.testing.assert_allclose(trimmed['hi'] - trimmed['lo'], SIDE / 2)
        self.assertTrue(mask.all())

    def test_samples_in_a_line_along_the_normal_still_give_a_card_plane(self):
        normal = np.eye(3)[0]
        points = np.outer(np.linspace(-CARD_EPSILON, CARD_EPSILON, SUBDIVISIONS), normal)
        card = card_from(np.zeros(3), normal, points, CARD_EPSILON)
        for axis in (card['a'], card['b']):
            self.assertTrue(np.all(np.isfinite(axis)))
            self.assertAlmostEqual(float(np.linalg.norm(axis)), 1.0)
            self.assertAlmostEqual(float(axis @ normal), 0.0)

    @needs_llvm
    def test_a_card_with_no_extent_bakes_to_nothing(self):
        p, t = quads()
        uv = p[:, :2] / SIDE + 0.5
        card = dict(choose_cards(p, t)[0])
        card['hi'] = card['lo'].copy()
        with np.errstate(all='raise'):
            sub = with_triangles(SimpleNamespace(p=p, uv=uv, textures=[None]), t, t, np.zeros(len(t), int))
            query = RayQuery(p, t, hit_filter=AlphaTest(sub, 0.5))
            self.assertIsNone(bake_card(card, query))

    @needs_llvm
    def test_long_card_splits(self):
        from r3d.cards import CARD_MAX_SIDE
        p, t = quads()
        card = choose_cards(p, t)[0]
        card['lo'] = np.zeros(2)
        card['hi'] = np.array([CARD_MAX_SIDE * 2.5, CARD_MAX_SIDE / 2])
        pieces, splits = split_cards([card])
        self.assertEqual(len(pieces), 3)
        self.assertEqual(splits, len(pieces) - 1)
        self.assertTrue(all(max(c['hi'] - c['lo']) <= CARD_MAX_SIDE for c in pieces))

    @needs_llvm
    def test_uniform_texture_and_mask_roundtrip(self):
        from PIL import Image
        p, t = quads()
        uv = p[:, :2] / SIDE + 0.5
        with tempfile.TemporaryDirectory() as directory:
            image = pathlib.Path(directory) / 'texture.png'
            colour = np.array([64, 128, 192], np.uint8)
            Image.new('RGBA', (8, 8), tuple(colour) + (255,)).save(image)
            texture = Texture(image)
            sub = with_triangles(SimpleNamespace(p=p, uv=uv, textures=[texture]), t, t, np.zeros(len(t), int))
            query = RayQuery(p, t, hit_filter=AlphaTest(sub, 0.5))
            baked = bake_card(choose_cards(p, t)[0], query)
        self.assertIsNotNone(baked)
        card, mask, albedo, coverage = baked
        np.testing.assert_allclose(corner_colours(mask, albedo, coverage),
                                   np.tile((colour / 255) ** 2.2, (4, 1)))
        self.assertTrue(mask.any())


    @needs_llvm
    def test_alpha_rejected_front_surface_reveals_back_and_empty_drops(self):
        from PIL import Image
        p, t = quads()
        # The first layer is transparent; the rear layer is fully opaque.
        p = np.concatenate([p, p - [0, 0, CARD_EPSILON / 2]])
        t = np.concatenate([t, t + len(p) // 2])
        uv = np.concatenate([np.tile([0.25, 0.5], (len(p) // 2, 1)), np.tile([0.75, 0.5], (len(p) // 2, 1))])
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / 'alpha.png'
            image = Image.new('RGBA', (8, 8), (255, 255, 255, 0))
            for x in range(4, 8):
                for y in range(8):
                    image.putpixel((x, y), (255, 255, 255, 255))
            image.save(path)
            tex = Texture(path)
            card = choose_cards(p, t)[0]
            sub = with_triangles(SimpleNamespace(p=p, uv=uv, textures=[tex]), t, t, np.zeros(len(t), int))
            query = RayQuery(p, t, hit_filter=AlphaTest(sub, 0.5))
            baked = bake_card(card, query)
            self.assertIsNotNone(baked)
            sub = with_triangles(sub, t, t, sub.tri_m, uv=np.tile([0.25, 0.5], (len(p), 1)))
            empty = bake_card(card, RayQuery(p, t, hit_filter=AlphaTest(sub, 0.5)))
            self.assertIsNone(empty)

    @needs_llvm
    def test_the_replaced_material_still_casts_light_but_not_visibility(self):
        from unittest import mock
        from r3d.import_settings import albedo_jobs
        from tests.bake_stubs import unlit_bake_geometry
        p, t = quads()
        bounced = []
        with tempfile.TemporaryDirectory() as directory:
            settings = card_settings(leaf_import(pathlib.Path(directory), p, t))
            geometry, built, shaded = unlit_bake_geometry(
                albedo_jobs(settings)[0], {}, lambda points, tris, *rest, **kw: mock.Mock(name=f"query of {len(tris)}"),
                path_light_for=lambda src, *rest: bounced.append(len(src.tri_v)), carve=True)
        from r3d.light import triangle_alpha
        from r3d.mesh_source import load_source
        with tempfile.TemporaryDirectory() as directory:
            source = load_source(card_settings(leaf_import(pathlib.Path(directory), p, t)))
        material = source.names.index('leaf')
        ids = source.tri_m == material
        opaque = triangle_alpha(source.uv, source.tri_t[ids], source.textures[material]) >= settings.alpha_keep
        solid = 1
        self.assertEqual(built[:2], [solid, solid + int(opaque.sum())])
        self.assertEqual(bounced, [built[1]])
        self.assertTrue(shaded)
        self.assertTrue(all(query is geometry.intersector for query in shaded))
        self.assertEqual(geometry.intersector._mock_name, f"query of {built[1]}")

    @needs_llvm
    def test_import_emits_masked_shape_and_atlas(self):
        from PIL import Image
        from r3d.cards.emit import make_shape
        from r3d.import_settings import load_import_settings, albedo_jobs
        from r3d.mesh_source import load_source
        from r3d import lod_asset
        p, t = quads()
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            settings_path = leaf_import(root, p, t)
            settings = card_settings(settings_path)
            src = load_source(settings)
            from r3d.cards.emit import material_views
            views, query = material_views(src, src.names.index('leaf'), settings, None)
            shape, report = make_shape(src, src.names.index('leaf'), settings.cards, settings.seed,
                                       settings.alpha_keep, views=views, query=query)
            job = albedo_jobs(settings)[0]
            geometry = mesh_import.bake_geometry(job, None, carve=True)
            self.assertEqual(report, {key: value for key, value in geometry.mesh_report['cards']['leaf'].items()
                                      if key in report})
            # A bundle name as long as a shape id can carry: the cards' id is named after their material instead.
            name = 'm' * (mesh_bundle.NAME_BYTES - len(mesh_bundle.NAME_SUFFIX) - mesh_bundle.HASH_HEX_DIGITS - 1)
            mesh_import.write_baked(job, None, root, name, geometry)
            entries, copies = mesh_bundle.decode((root / f'{name}{mesh_bundle.SUFFIX}').read_bytes())
            self.assertEqual(len(copies), 1)
            self.assertTrue(copies[0]['mesh'].startswith('leaf.'))
            rest = read_bundle_mesh(entries, name)
            blobs = {name: blob for name, _, blob in entries}
            self.assertNotIn(uv_transfer.name_for_mesh(name), blobs)
            mesh_id = copies[0]['mesh']
            masked = read_bundle_mesh(entries, mesh_id)
            self.assertTrue(all(cluster[-1] for cluster in masked.clusters))
            self.assertIsNone(masked.lod)
            self.assertEqual(len(masked.pos), 4 * report['cards'])
            self.assertFalse(any(name == lod_asset.name_for_mesh(mesh_id) for name, _, _ in entries))
            luvs = uv_transfer.decode(blobs[uv_transfer.name_for_mesh(mesh_id)])
            np.testing.assert_array_equal(luvs.codes, 1)
            np.testing.assert_array_equal(luvs.materials[:, 1:], uv_transfer.NONE)
            self.assertEqual(set(map(tuple, luvs.uvs)), set(map(tuple, shape['vertex_uvs'])))
            self.assertEqual(luvs.material_count, report['pages'])
            for page, expected in enumerate(shape['pages']):
                texture = texture_asset.decode(blobs[uv_transfer.texture_name(mesh_id, page)])
                self.assertEqual(texture.format, texture_asset.MASK1)
                self.assertEqual(len(texture.levels), len(expected.levels))
                for actual, reference in zip(texture.levels, expected.levels):
                    np.testing.assert_array_equal(actual, reference)

            lines = (root / 'source.obj').read_text().splitlines()[:-4] + ['v ' + ' '.join(map(str, row)) for row in p + [0, 0, SIDE]]
            lines += ['f ' + ' '.join(str(i + len(p) + 1) for i in tri) for tri in t]
            (root / 'source.obj').write_text('\n'.join(lines))
            settings_path.write_text('[source]\npath = "source.obj"\ncredit = "synthetic"\n[output]\n'
                '[geometry]\nalpha_mask = { keep_alpha = 0.5 }\n'
                f'simplify = {{ dense_edge = {SIDE}, props = [], props_share = 0.0, seal_seams = false, colour_deviation = 1.0 }}\n'
                f'[[variants]]\nname = "mesh"\ntriangles = {len(t)}\nceiling = {len(t)}\n')
            limited = albedo_jobs(card_settings(settings_path))[0]
            geometry = mesh_import.bake_geometry(limited, None, carve=True)
            limited_report = geometry.mesh_report['cards']['leaf']
            self.assertEqual({key: value for key, value in report.items() if key not in ('probes', 'held_out')},
                             {key: limited_report[key] for key in report if key not in ('probes', 'held_out')})
            self.assertEqual(limited_report['probes']['triangles'], 2 * len(t))
            self.assertLessEqual(geometry.mesh_report['scene_total'], limited.renderer.variant.ceiling)
            self.assertGreater(len(geometry.tris), 0)



if __name__ == '__main__':
    unittest.main()
