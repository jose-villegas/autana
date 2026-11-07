"""Synthetic billboard selection, alpha baking and LMSK emission."""
import pathlib
import sys
import tempfile
import unittest
from types import SimpleNamespace

import numpy as np

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from r3d.cards import CARD_EPSILON
from r3d.cards.choose import choose_cards, split_cards
from r3d.cards.bake import bake_card, corner_colours
from r3d import mask_asset, mesh_bundle, mesh_import
from r3d.obj import Texture
from tests import soup  # noqa: F401

SIDE = CARD_EPSILON * 12
SEEDS = range(3)
PATH_EVERY_MS = 250
NORMAL_TOLERANCE = 0.02


def quads(crossed=False):
    p = np.array([[-1, -1, 0], [1, -1, 0], [-1, 1, 0], [1, 1, 0]], float) * SIDE / 2
    t = np.array([[0, 1, 2], [2, 1, 3]])
    if crossed:
        p = np.concatenate([p, p[:, [2, 1, 0]]])
        t = np.concatenate([t, t + 4])
    return p, t


class CardTests(unittest.TestCase):
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

    def test_area_cost_can_stop_selection(self):
        p, t = quads(True)
        for seed in SEEDS:
            self.assertLessEqual(len(choose_cards(p, t, area_cost=100, seed=seed)),
                                 len(choose_cards(p, t, seed=seed)))

    def test_path_weights_unseen_quad_to_zero(self):
        p, t = quads()
        p = np.concatenate([p, p + [SIDE * 4, 0, 0]])
        t = np.concatenate([t, t + 4])
        poses = (mask_asset.MASK_WIDTH, mask_asset.MASK_WIDTH, 0.5, CARD_EPSILON,
                 [np.array([0, 0, SIDE, 0, 0, -1])])
        for seed in SEEDS:
            cards = choose_cards(p, t, seed=seed, path=poses)
            self.assertEqual(len(cards), 1)
            self.assertLess(abs(cards[0]['c'][0]), SIDE)

    def test_camera_clip_visibility_and_source_key(self):
        from r3d.cards.path import camera_path
        from r3d.import_settings import load_import_settings, source_digest
        p, t = quads()
        p = np.concatenate([p, p + [SIDE * 4, 0, 0]])
        t = np.concatenate([t, t + 4])
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            keys = root / 'camera.keys.toml'
            keys.write_text(f'node = "camera"\nanimation = "fly"\n'
                f'[[keys]]\nt = 0\neye = [0, 0, {SIDE}]\nlook_at = [0, 0, 0]\n'
                f'[[keys]]\nt = 1\neye = [0, 0, {SIDE}]\nlook_at = [0, 0, 0]\n')
            clip = root / 'fly.anim.toml'
            clip.write_text('source = "camera.keys.toml"\nanimation = "fly"\n')
            path = camera_path(clip, size=(mask_asset.MASK_WIDTH,) * 2, every_ms=PATH_EVERY_MS)
            self.assertEqual(len(choose_cards(p, t, path=path)), 1)
            (root / 'mesh.obj').write_text('')
            (root / 'mesh.mtl').write_text('')
            settings_file = root / 'mesh.import.toml'
            settings_file.write_text('[source]\npath = "mesh.obj"\ncredit = "synthetic"\n'
                '[output]\nname = "mesh"\n[geometry]\nalpha_mask = { keep_alpha = 0.5 }\n'
                'cards = { material = "leaf", choice = "path", path = "fly.anim.toml" }\n')
            settings = load_import_settings(settings_file)
            before = source_digest(settings)
            keys.write_text(keys.read_text().replace(f'[0, 0, {SIDE}]', f'[0, 0, {SIDE * 2}]'))
            self.assertNotEqual(before, source_digest(settings))

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

    def test_uniform_texture_and_mask_roundtrip(self):
        from PIL import Image
        p, t = quads()
        uv = np.array([[0, 0], [1, 0], [0, 1], [1, 1]], float)
        with tempfile.TemporaryDirectory() as directory:
            image = pathlib.Path(directory) / 'texture.png'
            colour = np.array([64, 128, 192], np.uint8)
            Image.new('RGBA', (8, 8), tuple(colour) + (255,)).save(image)
            texture = Texture(image)
            baked = bake_card(choose_cards(p, t)[0], p, t, uv, t, texture, 0.5)
        self.assertIsNotNone(baked)
        card, mask, albedo, coverage = baked
        np.testing.assert_allclose(corner_colours(mask, albedo, coverage),
                                   np.tile((colour / 255) ** 2.2, (4, 1)))
        self.assertTrue(mask.any())


    def test_alpha_rejected_front_surface_reveals_back_and_empty_drops(self):
        from PIL import Image
        p, t = quads()
        # The first layer is transparent; the rear layer is fully opaque.
        p = np.concatenate([p, p - [0, 0, CARD_EPSILON / 2]])
        t = np.concatenate([t, t + 4])
        uv = np.concatenate([np.tile([0.25, 0.5], (4, 1)), np.tile([0.75, 0.5], (4, 1))])
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / 'alpha.png'
            image = Image.new('RGBA', (8, 8), (255, 255, 255, 0))
            for x in range(4, 8):
                for y in range(8):
                    image.putpixel((x, y), (255, 255, 255, 255))
            image.save(path)
            tex = Texture(path)
            card = choose_cards(p, t)[0]
            baked = bake_card(card, p, t, uv, t, tex, 0.5)
            self.assertIsNotNone(baked)
            empty = bake_card(card, p, t, np.tile([0.25, 0.5], (len(p), 1)), t, tex, 0.5)
            self.assertIsNone(empty)

    def test_import_emits_masked_shape_and_atlas(self):
        from PIL import Image
        from r3d.cards.emit import make_shape
        from r3d.import_settings import load_import_settings, albedo_jobs
        from r3d.mesh_source import load_source
        from r3d import lod_asset
        p, t = quads()
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            checker = np.zeros((8, 8, 4), dtype=np.uint8)
            checker[..., :3] = [64, 128, 192]
            checker[..., 3] = (np.indices(checker.shape[:2]).sum(axis=0) % 2) * 255
            Image.fromarray(checker).save(root / 'leaf.png')
            (root / 'source.mtl').write_text('newmtl leaf\nKd 1 1 1\nmap_Kd leaf.png\nnewmtl solid\nKd 1 1 1\n')
            lines = ['mtllib source.mtl'] + ['v ' + ' '.join(map(str, row)) for row in p]
            lines += ['vt 0 0', 'vt 1 0', 'vt 0 1', 'vt 1 1', 'o leaf_object', 'usemtl leaf',
                      'f 1/1 2/2 3/3', 'f 3/3 2/2 4/4', 'o solid_object', 'usemtl solid',
                      f'v 0 0 {SIDE}', f'v {SIDE} 0 {SIDE}', f'v 0 {SIDE} {SIDE}', 'f 5 6 7']
            (root / 'source.obj').write_text('\n'.join(lines))
            settings_path = root / 'mesh.import.toml'
            settings_path.write_text('[source]\npath = "source.obj"\ncredit = "synthetic"\n'
                '[output]\nname = "mesh"\n[geometry]\nalpha_mask = { keep_alpha = 0.5 }\ncards = { material = "leaf" }\n')
            settings = load_import_settings(settings_path)
            src = load_source(settings)
            shape, report = make_shape(src, src.names.index('leaf'), settings.cards, settings.seed, settings.alpha_keep)
            job = albedo_jobs(settings)[0]
            geometry = mesh_import.bake_geometry(job, None, carve=True)
            self.assertEqual(report, geometry.mesh_report['cards'])
            mesh_import.write_baked(job, None, root, 'mesh', geometry)
            entries, copies = mesh_bundle.decode((root / 'mesh.bundle').read_bytes())
            self.assertEqual(len(copies), 1)
            rest = mesh_bundle.read_mesh(entries, 'mesh')
            self.assertFalse(any(rest.masked))
            mesh_id = copies[0]['mesh']
            masked = mesh_bundle.read_mesh(entries, mesh_id)
            self.assertTrue(all(masked.masked))
            self.assertTrue(all(cluster[-1] for cluster in masked.clusters))
            self.assertIsNone(masked.lod)
            self.assertEqual(masked.mask_rows, shape['mask_rows'])
            self.assertEqual(len(masked.pos), 4 * report['cards'])
            self.assertFalse(any(name == lod_asset.name_for_mesh(mesh_id) for name, _, _ in entries))
            self.assertEqual(set(map(tuple, masked.mask_uvs)), set(map(tuple, shape['mask_uvs'])))
            first, _, rows = mask_asset.decode(dict((name, blob) for name, _, blob in entries)[
                mask_asset.name_for_mesh(mesh_id)], len(masked.pos))
            self.assertEqual(first, 0)
            self.assertEqual(rows, shape['mask_rows'])



if __name__ == '__main__':
    unittest.main()
