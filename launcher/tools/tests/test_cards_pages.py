"""Card atlases keep independent masks through packing, mip generation and LUVS encoding."""
import pathlib
import sys
import unittest

import numpy as np

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from soup import Soup  # noqa: E402
from r3d.cards import CARD_MIP_LEVELS
from r3d.cards.pages import pack_pages
from r3d.mesh_source import with_triangles
from r3d.texture import texture_asset, uv_transfer


class PageTests(unittest.TestCase):
    def test_padding_nonoverlap_and_overflow(self):
        side = 1 << texture_asset.SIZE_LOG2_MAX
        padding = 1 << (CARD_MIP_LEVELS - 1)
        masks = ([np.ones((side // 2, side // 2), bool) for _ in range(5)] +
                 [np.ones((side // 4, side // 4), bool) for _ in range(8)])
        pages, rects = pack_pages(masks)
        self.assertGreater(len(pages), 1)
        occupied = np.zeros((len(pages), side, side), bool)
        for mask, (page, x, y, width, height) in zip(masks, rects):
            self.assertGreaterEqual(x, padding)
            self.assertGreaterEqual(y, padding)
            region = occupied[page, y-padding:y+height+padding, x-padding:x+width+padding]
            self.assertFalse(region.any())
            region[:] = True
            np.testing.assert_array_equal(pages[page][y:y+height, x:x+width], mask)
            self.assertFalse(pages[page][y-padding:y, x-padding:x+width+padding].any())
            self.assertFalse(pages[page][y:y+height, x-padding:x].any())
        self.assertLess(len(pages), len(masks))

    def test_mask1_mips_and_roundtrip(self):
        side = 1 << texture_asset.MASK1_SIZE_LOG2_MIN
        bits = np.zeros((side * 2, side * 2), bool)
        for count in range(5):
            y, x = divmod(count, side)
            block = bits[y*2:y*2+2, x*2:x*2+2]
            block.flat[:count] = True
        indexed = texture_asset.mask_texture(bits, 2)
        mip = np.asarray(indexed.levels[1])
        for count in range(5):
            y, x = divmod(count, side)
            self.assertEqual(bool(mip[y, x]), count >= texture_asset.MASK1_MIP_KEPT)
        decoded = texture_asset.decode(texture_asset.pack_ltex(indexed))
        self.assertEqual(decoded.format, texture_asset.MASK1)
        self.assertEqual(decoded.palette[1], (texture_asset.ONE,) * texture_asset.TINT_RGB)
        np.testing.assert_array_equal(decoded.levels[0], bits)
        np.testing.assert_array_equal(decoded.levels[1], mip)

    def test_card_luvs_chart_zero_is_page_and_every_code_is_one(self):
        from types import SimpleNamespace
        mesh = SimpleNamespace(pos=np.zeros((8, 3)), tris=np.zeros((4, 3), int),
                               clusters=[(0, 4, 0, 2), (4, 4, 2, 2)])
        uvs = np.arange(16).reshape(8, 2)
        luvs = uv_transfer.card_luvs(mesh, uvs, [1, 0])
        decoded = uv_transfer.decode(uv_transfer.pack_luvs(luvs))
        np.testing.assert_array_equal(decoded.materials[:, 0], [1, 0])
        np.testing.assert_array_equal(decoded.materials[:, 1:], uv_transfer.NONE)
        np.testing.assert_array_equal(decoded.codes, 1)
        np.testing.assert_array_equal(decoded.uvs, uvs)

    def test_page_labels_and_uvs_survive_welding_and_meshlet_order(self):
        from r3d.lit_mesh import bake_lit_mesh
        quad = Soup([[0, 0, 0], [1, 0, 0], [0, 1, 0], [1, 1, 0]], [[0, 1, 2], [2, 1, 3]])
        tris = np.concatenate([quad.faces, quad.faces + len(quad.vertices)])
        uv_scale = 1 << uv_transfer.UV_SHIFT
        corners = np.asarray(quad.vertices)[:, :2] * uv_scale
        page_offset = np.array([2, 3]) * uv_scale
        uvs = np.concatenate([corners + page_offset, corners])
        pages = np.repeat([1, 0], 4)
        mesh = bake_lit_mesh(np.tile(quad.vertices, (2, 1)), np.full((8, 3), 128), tris, np.ones(4),
                             vertex_uvs=uvs, vertex_pages=pages)
        self.assertEqual(len(mesh.pos), 8)
        self.assertEqual(len(mesh.tris), 4)
        luvs = uv_transfer.card_luvs(mesh, mesh.vertex_uvs, mesh.page_of_cluster)
        self.assertEqual(set(luvs.materials[:, 0]), {0, 1})
        np.testing.assert_array_equal(luvs.codes, 1)
        for cluster, (vb, vc, tb, tc, *_) in enumerate(mesh.clusters):
            self.assertEqual(vc, 4)
            self.assertEqual(tc, 2)
            page = int(luvs.materials[cluster, 0])
            positions = mesh.pos[vb:vb + vc] / mesh.position_scale
            expected_uvs = positions[:, :2] * uv_scale + page * page_offset
            np.testing.assert_array_equal(luvs.uvs[vb:vb + vc], expected_uvs)
            first = 2 if page == 0 else 0
            expected_faces = sorted(tuple(sorted(map(tuple, uvs[face]))) for face in tris[first:first + 2])
            faces = mesh.tris[tb:tb + tc]
            actual_faces = sorted(tuple(sorted(map(tuple, luvs.uvs[face]))) for face in faces)
            self.assertEqual(actual_faces, expected_faces)



class CardSamplingTests(unittest.TestCase):
    def test_a_wall_hides_samples_but_a_cut_texel_passes_them(self):
        import tempfile
        from r3d.cards.choose import probe_weights, VISIBILITY_MARGIN
        from r3d.light import AlphaTest
        from r3d.ray_query import RayQuery
        from tests.test_r3d_alpha_cutout import checker_source, checker_rays, KEEP_ALPHA, SOLID, PLANE_Z, EYE_Z
        with tempfile.TemporaryDirectory() as directory:
            source = checker_source(directory, plane=False)
            origins, _ = checker_rays()
            points = origins.copy()
            points[:, 2] = PLANE_Z - VISIBILITY_MARGIN
            normals = np.tile([0., 0., 1.], (len(points), 1))
            faces = [np.array([0., 0., EYE_Z, 0., 0., -1.])]
            wall = RayQuery(source.p, source.tri_v, "llvm_ad_rgb")
            np.testing.assert_array_equal(probe_weights(points, faces, wall, 1.0, near_floor=0.), 0)
            cut = RayQuery(source.p, source.tri_v, "llvm_ad_rgb", hit_filter=AlphaTest(source, KEEP_ALPHA))
            weight = probe_weights(points, faces, cut, 1.0, near_floor=0.)
            np.testing.assert_array_equal(weight[np.asarray(SOLID)], 0)
            self.assertTrue((weight[~np.asarray(SOLID)] > 0).all())

    def test_slab_rejects_hits_outside_epsilon(self):
        from r3d.cards import CARD_EPSILON
        from r3d.cards.bake import bake_pixels
        from r3d.light import AlphaTest
        from types import SimpleNamespace
        from r3d.ray_query import RayQuery
        epsilon = CARD_EPSILON
        card = dict(c=np.zeros(3), a=np.eye(3)[0], b=np.eye(3)[1], n=np.eye(3)[2],
                    lo=np.full(2, -1.), hi=np.full(2, 1.), depth=(-epsilon, epsilon), texel_size=0.25)
        tris = np.array([[0, 1, 2], [2, 1, 3]])
        for z, kept in ((-epsilon * 1.1, False), (-epsilon * .9, True),
                        (epsilon * 1.1, False), (epsilon * .9, True)):
            positions = np.array([[-1., -1., z], [1., -1., z], [-1., 1., z], [1., 1., z]])
            sub = with_triangles(SimpleNamespace(p=positions, textures=[None]),
                                 tris, tris, np.zeros(len(tris), int))
            query = RayQuery(positions, tris, "llvm_ad_rgb", hit_filter=AlphaTest(sub, .5))
            mask, _, _ = bake_pixels(card, query)
            self.assertEqual(bool(mask.any()), kept, z)

    def test_one_density_for_a_set_survives_splitting(self):
        from r3d.cards import CARD_MASK_TEXELS
        from r3d.cards.choose import split_cards
        cards = [dict(lo=np.zeros(2), hi=np.array([side, side / 2])) for side in (16., 32., 64.)]
        pieces, _ = split_cards(cards, max_side=16.)
        self.assertEqual({piece['texel_size'] for piece in pieces}, {32. / CARD_MASK_TEXELS})

    def test_bare_probe_region_and_near_follow_source_scale(self):
        from unittest.mock import patch
        from r3d.views import probes
        positions = np.array([[0., 0., 0.], [2., 3., 4.], [2., 0., 0.]])
        triangles = np.array([[0, 1, 2]])
        near = np.linalg.norm(np.ptp(positions, axis=0)) * probes.PROBE_NEAR_SHARE
        with patch.object(probes, 'classify', side_effect=lambda q, n, p, c, z: (p, np.full(len(p), 'inside'))) as classify:
            for scale in (1., 100.):
                views = probes.select(positions * scale, triangles, None, None, None, 1, query=object())
                self.assertAlmostEqual(classify.call_args.args[-1], near * scale)
                self.assertGreaterEqual(views.record['spacing'], probes.PROBE_SPACING_NEAR_PLANES * near * scale)
                np.testing.assert_allclose(views.record['region'], [positions.min(axis=0) * scale,
                                                                    positions.max(axis=0) * scale])
