import math
import pathlib
import struct
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from anim import skeleton_asset
from r3d import skin_asset


class Formats(unittest.TestCase):
    def test_two_roots_round_trip(self):
        rows = [('a', 255, (0, 0, 0, 0, 0, 0, 1, 1, 1, 1)),
                ('b', 255, (0, 0, 0, 0, 0, 0, 1, 1, 1, 1))]
        self.assertEqual(skeleton_asset.decode(skeleton_asset.encode(rows)), rows)

    def test_skeleton_refusals(self):
        rest = (0, 0, 0, 0, 0, 0, 1, 1, 1, 1)
        for rows in ([], [('a', 0, rest)], [('a', 255, rest), ('a', 255, rest)],
                     [('a', 255, rest[:-1] + (math.inf,))],
                     [('a', 255, rest[:6] + (2,) + rest[7:])]):
            with self.assertRaises(ValueError):
                skeleton_asset.encode(rows)

    def test_skin_round_trip(self):
        matrix = (1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0)
        vertices = [((0, 0), (255, 0), (0, 127, 0))]
        self.assertEqual(skin_asset.decode(skin_asset.encode([matrix], vertices, 2)),
                         ([matrix], vertices, 2))

    def test_weight_ties_and_sum(self):
        self.assertEqual(skin_asset.quantize_weights((3, 2, 1, 0), (1, 1, 1, 1), 4),
                         ((0, 1, 2, 3), (64, 64, 64, 63)))
        self.assertEqual(sum(skin_asset.quantize_weights((0, 1, 2, 3), (1, 2, 3, 4), 2)[1]), 255)


class Matching(unittest.TestCase):
    def test_weld_seam_weights_differ(self):
        with self.assertRaisesRegex(ValueError, r'weight disagreement.*vertex 0.*\(0, 0, 0\)'):
            skin_asset.match_vertices([(0, 0, 0)], 8, [(0, 0, 0)] * 2, [(0, 1, 0)] * 2,
                                      [(0, 1)] * 2, [(1, 0), (0, 1)])

    def test_matching_refusals(self):
        for positions, normals, weights, message in (
                ([(1, 0, 0)], [(0, 1, 0)], [(1, 0)], 'no source match'),
                ([(0, 0, 0)], [(0, 0, 0)], [(1, 0)], 'mean normal'),
                ([(0, 0, 0)], [(0, 1, 0)], [(0, 0)], 'zero source weights')):
            with self.assertRaisesRegex(ValueError, message):
                skin_asset.match_vertices([(0, 0, 0)], 8, positions, normals, [(0, 1)], weights)

    def test_quantizer_matches_mesh_owner(self):
        import skin_probe
        positions, scale = skin_asset.mesh_positions(skin_probe.mesh_entry())
        self.assertEqual(set(positions), {skin_asset.quantize_position(p, scale) for p in skin_probe.POSITIONS})
        from r3d.lit_mesh import bake_lit_mesh
        source = [(-0.1875, 0, 0), (1.03125, 0, 0), (0, 1.09375, 0)]
        mesh = bake_lit_mesh(source, [(255, 255, 255)] * 3, [(0, 1, 2)], [0], position_scale=16)
        self.assertEqual({tuple(map(int, p)) for p in mesh.pos},
                         {skin_asset.quantize_position(p, 16) for p in source})

    def test_matrix_node_is_refused_by_name(self):
        import skin_probe
        document, binary = skin_probe.rig()
        document['nodes'][0]['matrix'] = skin_probe.IDENTITY
        with self.assertRaisesRegex(ValueError, "node 'armature'.*matrix"):
            skin_asset.bake(document, binary, 5, skin_probe.mesh_entry())

    def test_mesh_node_model_space_must_match(self):
        import skin_probe
        document, binary = skin_probe.rig()
        document['nodes'][5]['translation'] = [1, 0, 0]
        with self.assertRaisesRegex(ValueError, "root 'a'.*mesh node 'mesh'"):
            skin_asset.bake(document, binary, 5, skin_probe.mesh_entry())

    def test_end_to_end_decoded_entries(self):
        import skin_probe
        from anim import tracks_asset
        from r3d import gltf_skin
        document, binary = skin_probe.rig()
        reference = gltf_skin.SkinnedAsset(document, binary)
        measured = {}
        for influences in (2, 4):
            entries = skin_probe.entries(influences)
            skeleton = skeleton_asset.decode(entries[0][2])
            inverse, vertices, width = skin_asset.decode(entries[1][2])
            tracks, _ = tracks_asset.decode(entries[2][2])
            positions, scale = skin_asset.mesh_positions(skin_probe.mesh_entry())
            error = 0
            for frame in range(61):
                seconds = frame / 60
                pose = []
                for path, parent, rest in skeleton:
                    trs = dict(translation=rest[:3], rotation=rest[3:7], scale=rest[7:])
                    for track in tracks:
                        if track['path'] == path:
                            field = {'position': 'translation', 'rotation': 'rotation', 'scale': 'scale'}[track['field']]
                            trs[field] = tracks_asset.sample(track, seconds)
                    pose.append(trs)
                parents = [None if parent == skeleton_asset.ROOT else parent for _, parent, _ in skeleton]
                world = gltf_skin.world_matrices(parents, pose)
                matrices = [gltf_skin.mat_mul(m, [*ib, 0, 0, 0, 1]) for m, ib in zip(world, inverse)]
                want, _ = reference.skin(reference.sample('move', seconds))
                for pos, (joints, weights, _) in zip(positions, vertices):
                    p = tuple(v / scale for v in pos)
                    got = [sum(gltf_skin.transform_point(matrices[j], p)[axis] * w / skin_asset.WEIGHT_SUM
                               for j, w in zip(joints, weights)) for axis in range(3)]
                    source_index = skin_probe.POSITIONS.index(p)
                    error = max(error, max(abs(a - b) for a, b in zip(got, want[source_index])))
            measured[influences] = error
        self.assertLessEqual(measured[4], 1e-6)
        self.assertGreater(measured[2], measured[4])
        print(f'probe positions: max error 4={measured[4]:.9g}, 2={measured[2]:.9g} model units; 61 frames')


class Refusals(unittest.TestCase):
    def test_skeleton_binary_refusals(self):
        import skin_probe
        entry = skin_probe.entries()[0][2]
        # Each mutation isolates one open-time refusal on a valid Python entry.
        changes = [(0, '<H', 2), (2, 'B', 0), (2, 'B', 255), (3, 'B', 1),
                   (4, '<I', 17), (4, '<I', 0xffffffff), (8, '<I', 0xffffffff),
                   (12, '<I', 0), (12, '<I', 17), (18, 'B', 0), (19, 'B', 1),
                   (20, '<H', 0), (16, '<H', 65535)]
        rest_off = struct.unpack_from('<I', entry, 12)[0]
        changes += [(rest_off, '<f', math.inf), (rest_off + 24, '<f', 2)]
        for offset, pattern, value in changes:
            with self.subTest(offset=offset, value=value):
                bad = bytearray(entry)
                struct.pack_into(pattern, bad, offset, value)
                with self.assertRaises(ValueError):
                    skeleton_asset.decode(bad)
        for cut in (0, 15, len(entry) - 1):
            with self.assertRaises(ValueError):
                skeleton_asset.decode(entry[:cut])
        bad = bytearray(entry); bad[-1] = ord('x')
        with self.assertRaises(ValueError):
            skeleton_asset.decode(bad)

    def test_skin_binary_refusals(self):
        import skin_probe
        entry = skin_probe.entries()[1][2]
        vertices = struct.unpack_from('<I', entry, 12)[0]
        changes = [(0, '<H', 2), (2, 'B', 0), (2, 'B', 255), (3, 'B', 3),
                   (4, '<I', 0xffffffff), (8, '<I', 0), (8, '<I', 17),
                   (12, '<I', 0), (12, '<I', 17), (16, '<f', math.nan),
                   (vertices, 'B', 254), (vertices + 4, 'B', 0), (vertices + 11, 'B', 1),
                   (vertices + 8, 'b', -128)]
        for offset, pattern, value in changes:
            with self.subTest(offset=offset, value=value):
                bad = bytearray(entry); struct.pack_into(pattern, bad, offset, value)
                with self.assertRaises(ValueError):
                    skin_asset.decode(bad)
        for cut in (0, 15, len(entry) - 1):
            with self.assertRaises(ValueError):
                skin_asset.decode(entry[:cut])

    def test_unit_tolerance_adjacent_f32_boundary(self):
        tolerance = skeleton_asset.ANIM_SKELETON_UNIT_TOLERANCE
        for sign in (-1, 1):
            bits = struct.unpack('<I', struct.pack('<f', math.sqrt(1 + sign * tolerance)))[0]
            candidates = [struct.unpack('<f', struct.pack('<I', b))[0] for b in (bits - 1, bits, bits + 1)]
            accepted = rejected = False
            for q in candidates:
                rest = (0, 0, 0, 0, 0, 0, q, 1, 1, 1)
                if abs(1 - q * q) <= tolerance:
                    skeleton_asset.encode([('root', 255, rest)]); accepted = True
                else:
                    with self.assertRaises(ValueError):
                        skeleton_asset.encode([('root', 255, rest)])
                    rejected = True
            self.assertTrue(accepted and rejected)

    def test_skin_writer_refusals(self):
        matrix = (1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0)
        for matrices, vertices, influences in (([], [], 2), ([matrix], [], 3),
                ([matrix], [((1, 0), (255, 0), (0, 127, 0))], 2),
                ([matrix], [((0, 0), (254, 0), (0, 127, 0))], 2),
                ([matrix], [((0, 0), (255, 0), (0, 0, 0))], 2),
                ([matrix[:-1] + (math.inf,)], [], 4)):
            with self.assertRaises(ValueError):
                skin_asset.encode(matrices, vertices, influences)

class PackStep(unittest.TestCase):
    def test_scene_pack_has_skin_and_rig(self):
        import tempfile
        import skin_probe
        from gltf import gltf_write
        from r3d import build_pack
        from asset.asset_pack import parse_pack
        document, binary = skin_probe.rig()
        # Write the probe with the shared glTF writer so its source is portable.
        nodes = document['nodes']
        primitive = dict(positions=skin_probe.POSITIONS, indices=[0, 1, 2], normals=skin_probe.NORMALS,
                         joints=skin_probe.JOINTS, weights=skin_probe.WEIGHTS)
        data = gltf_write.build_glb(nodes, [], meshes=[{'primitives': [primitive]}],
                skins=[{'joints': [1, 2, 3, 4], 'inverse_binds': [skin_probe.IDENTITY] * 4}])
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory)
            (path / 'rig.glb').write_bytes(data)
            (path / 'rig.import.toml').write_text('[source]\npath="rig.glb"\ncredit="probe"\n[output]\nname="rig"\n')
            (path / 'room.scene.toml').write_text('[[objects]]\nname="rig"\n[objects.mesh_renderer]\nmesh="rig.import.toml"\n')
            packs, _ = build_pack.pack_jobs([path])
            mesh_id = next(key for key, source in packs['room'].items() if source.suffix == '.mesh')
            mesh_source = packs['room'][mesh_id]
            mesh_source.write_bytes(skin_probe.mesh_entry())
            decoded = parse_pack(build_pack.pack_bytes([path], offline=True)['room'])
            self.assertEqual(decoded['armature'][0], skeleton_asset.TYPE)
            self.assertEqual(decoded[mesh_id + '.skin'][0], skin_asset.TYPE)
            self.assertNotIn('move', decoded)

    def test_ids_and_duplicate_sources(self):
        from r3d import skin_pack
        rows, sources = [], {}
        for key in ('x' * 32, 'Ã©' * 16):
            with self.assertRaisesRegex(ValueError, key):
                skin_pack.append(rows, [(key, b'SKEL', b'', 'source')], sources)
        skin_pack.append(rows, [('rig', b'SKEL', b'', 'source')], sources)
        skin_pack.append(rows, [('rig', b'SKEL', b'', 'source')], sources)
        self.assertEqual(len(rows), 1)
        with self.assertRaisesRegex(ValueError, 'source.*other'):
            skin_pack.append(rows, [('rig', b'SKEL', b'', 'other')], sources)


if __name__ == '__main__':
    unittest.main()
