"""Attribute visible reference and baked triangles to source materials and count their differences."""
from collections import defaultdict
import hashlib
import json
import pathlib

import numpy as np

from asset.asset_pack import parse_pack
from render import reference_poses
from asset.engine_frame import AXIS_COUNT, mirror_matrix, mirror_point
from r3d import mesh_bundle
from r3d.fit import gbuffer
from r3d.lit_mesh import read_bundle_mesh
from r3d.objects.instances import place
from r3d.texture import uv_transfer

EMPTY = -1
BOTH_EMPTY, AGREE_TEXTURED, AGREE_PLAIN, MISS, WRONG_MATERIAL, REFERENCE_ONLY, DRAWN_ONLY = range(7)
CLASS_NAMES = ('both_empty', 'agree_textured', 'agree_plain', 'miss', 'wrong_material', 'reference_only', 'drawn_only')


def source_ids(source, query, views, placement=None):
    """Top-first int32 source material maps traced through the caller's alpha-tested query."""
    result = []
    for matrix, (width, height) in views:
        if placement is not None:
            axes = AXIS_COUNT
            world = np.eye(axes + 1)
            world[:axes, :axes], world[:axes, axes] = placement.matrix, placement.position
            matrix = matrix @ world
        _depth, shown = gbuffer.traced_buffer(query, matrix, (width, height))
        material = np.full(shown.shape, EMPTY, dtype=np.int32)
        keep = shown >= 0
        material[keep] = np.asarray(source.tri_m)[shown[keep]]
        result.append(material[::-1].copy())
    return result


def source_points(points, placement):
    """World points returned to the renderer's source frame through its authored placement."""
    inverse = np.linalg.inv(np.asarray(placement.matrix))
    return place(np.asarray(points) - placement.position, inverse, np.zeros(AXIS_COUNT))


def entry_points(mesh, row, placement=None):
    """Bundle vertices placed by the engine's drawn transform, in source axes."""
    matrix = mirror_matrix(np.asarray(row['matrix']))
    position = mirror_point(row['position'])
    points = place(np.asarray(mesh.pos, dtype=np.float64) / float(mesh.position_scale), matrix, position)
    return points if placement is None else source_points(points, placement)


def drawn_ids(ids_dir, bundle_path, source, placement=None, texture_pack=None):
    """Source material and textured maps for every captured triangle-id frame."""
    root = pathlib.Path(ids_dir)
    metadata = json.loads((root / 'entries.json').read_text())
    bundle = pathlib.Path(bundle_path).read_bytes()
    pack = pathlib.Path(texture_pack).read_bytes() if texture_pack else None
    expected_bundle = hashlib.sha256(bundle).hexdigest()
    expected_texture = hashlib.sha256(pack).hexdigest() if pack is not None else None
    paths = sorted((int(path.stem), path) for path in root.glob('*.ids'))
    poses = reference_poses.render_poses(root / 'capture')
    if poses is None or [frame for frame, _ in paths] != sorted(poses):
        raise ValueError('id files must match the frames in the capture pose log')
    if sorted(poses) != list(range(len(poses))):
        raise ValueError('captured frames must start at zero without gaps')

    frames = defaultdict(list)
    for row in metadata['entries']:
        if row['frame'] not in poses:
            raise ValueError('captured entry is outside the capture pose log')
        if row.get('bundle_sha256') != expected_bundle:
            raise ValueError(f"{row['entry']}: capture bundle SHA-256 differs from the reference bundle")
        if 'texture_sha256' not in row or row['texture_sha256'] != expected_texture:
            raise ValueError(f"{row['entry']}: capture texture pack SHA-256 differs from the reference pack")
        frames[row['frame']].append(row)
    if not frames:
        raise ValueError('id capture has no entries identifying the drawn bundle')

    entries, _copies = mesh_bundle.decode(bundle)
    textures = parse_pack(pack) if pack is not None else {}
    meshes, mappings = {}, {}
    search = uv_transfer.material_search(source)
    materials, textured = [], []
    for frame, path in paths:
        ids = np.fromfile(path, '<u4').reshape(metadata['height'], metadata['width'])
        material = np.full(ids.shape, EMPTY, dtype=np.int32)
        pattern = np.zeros(ids.shape, dtype=bool)
        covered = ids == 0
        next_id = 1
        for row in frames.get(frame, ()):
            if row['step'] != 'triangle':
                raise ValueError('material attribution requires --view triangles')
            name = row['entry']
            if name not in meshes:
                mesh = read_bundle_mesh(entries, name)
                uv = textures.get(uv_transfer.name_for_mesh(name))
                codes = None
                if uv is not None:
                    if uv[0] != uv_transfer.TYPE:
                        raise ValueError('texture sibling is not LUVS')
                    codes = uv_transfer.decode(uv[1]).codes
                    if len(codes) != len(mesh.tris):
                        raise ValueError('texture codes differ from bundle triangles')
                meshes[name] = mesh, codes
            mesh, codes = meshes[name]
            key = (name, tuple(map(tuple, row['matrix'])), tuple(row['position']))
            if key not in mappings:
                points = entry_points(mesh, row, placement)
                mappings[key] = uv_transfer.nearest_materials(points, mesh.tris, source, search)[1]
            source_material = mappings[key]
            if row['triangle_count'] != len(source_material) or row['id_count'] != len(source_material):
                raise ValueError('captured triangle count differs from bundle')
            first_id = row['first_id']
            if first_id < next_id:
                raise ValueError('captured id ranges overlap')
            if first_id > next_id:
                raise ValueError('captured id ranges have a gap')
            next_id += row['id_count']
            own = (ids >= first_id) & (ids < next_id)
            local = ids[own].astype(np.int64) - first_id
            material[own] = source_material[local]
            if codes is not None:
                pattern[own] = codes[local] != uv_transfer.PLAIN
            covered |= own
        if not covered.all():
            raise ValueError(f'{path}: id outside captured entry ranges')
        materials.append(material)
        textured.append(pattern)
    return materials, textured


def diff_classes(ref, drawn, textured, has_texture):
    """One mutually exclusive class per pixel, including both kinds of empty pixel."""
    ref, drawn, textured = np.asarray(ref), np.asarray(drawn), np.asarray(textured)
    if ref.shape != drawn.shape or ref.shape != textured.shape:
        raise ValueError('material maps differ in size')
    result = np.full(ref.shape, BOTH_EMPTY, dtype=np.uint8)
    occupied = (ref != EMPTY) & (drawn != EMPTY)
    agree = occupied & (ref == drawn)
    result[agree & textured] = AGREE_TEXTURED
    result[agree & ~textured] = AGREE_PLAIN
    has_texture = np.asarray(has_texture, dtype=bool)
    wants = np.zeros(ref.shape, dtype=bool)
    wants[ref != EMPTY] = has_texture[ref[ref != EMPTY]]
    result[agree & ~textured & wants] = MISS
    result[occupied & ~agree] = WRONG_MATERIAL
    result[(ref != EMPTY) & (drawn == EMPTY)] = REFERENCE_ONLY
    result[(ref == EMPTY) & (drawn != EMPTY)] = DRAWN_ONLY
    return result


def material_table(references, drawn, classes, names):
    """CSV counts by reference material, with drawn-only pixels under their drawn material."""
    owners = [np.where(ref != EMPTY, ref, own) for ref, own in zip(references, drawn)]
    rows = []
    for material in range(EMPTY, len(names)):
        row = dict(material=material, name=names[material] if material != EMPTY else '(empty)')
        for code, name in enumerate(CLASS_NAMES):
            row[name] = sum(int(np.count_nonzero((ref == material) & (diff == code)))
                            for ref, diff in zip(owners, classes))
        rows.append(row)
    return rows


def material_confusions(references, drawn, classes):
    """Wrong-material counts by source material pair, sorted by decreasing pixel count."""
    from collections import Counter

    counts = Counter()
    for ref, own, diff in zip(references, drawn, classes):
        wrong = diff == WRONG_MATERIAL
        pairs, pixels = np.unique(np.column_stack([ref[wrong], own[wrong]]), axis=0, return_counts=True)
        counts.update({tuple(pair): int(count) for pair, count in zip(pairs, pixels)})
    return [dict(reference_material=int(pair[0]), drawn_material=int(pair[1]), pixels=count)
            for pair, count in sorted(counts.items(), key=lambda row: (-row[1], row[0]))]
