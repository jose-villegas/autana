"""Attribute visible reference and baked triangles to source materials and count their differences."""
import json
import pathlib

import numpy as np

from asset.asset_pack import parse_pack
from asset.engine_frame import AXIS_SIGNS
from r3d import mesh_bundle
from r3d.fit import gbuffer
from r3d.lit_mesh import read_bundle_mesh
from r3d.poses import read_poses
from r3d.objects.instances import place
from r3d.texture import uv_transfer

EMPTY = -1
BOTH_EMPTY, AGREE_TEXTURED, AGREE_PLAIN, MISS, WRONG_MATERIAL, ONE_EMPTY = range(6)
CLASS_NAMES = ('both_empty', 'agree_textured', 'agree_plain', 'miss', 'wrong_material', 'one_empty')


def source_ids(source, query, poses_path, size=None):
    """Top-first int32 source material maps traced through the caller's alpha-tested query."""
    from r3d.appearance_simplify import projection

    if str(poses_path).endswith('.csv'):
        import sys
        render = pathlib.Path(__file__).resolve().parents[1] / 'render'
        if str(render) not in sys.path:
            sys.path.insert(0, str(render))
        from reference_poses import capture_poses
        width, height, lens, near, poses = capture_poses(poses_path, size)
    else:
        width, height, lens, near, poses = read_poses(poses_path)
    if size is not None:
        width, height = size
    result = []
    for pose in poses:
        matrix = projection(pose[:3], pose[3:], width, height, lens, near)
        placement = getattr(source, 'material_placement', None)
        if placement is not None:
            axes = len(AXIS_SIGNS)
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
    return place(np.asarray(points) - placement.position, inverse, np.zeros(len(AXIS_SIGNS)))


def entry_points(mesh, row, placement=None):
    """Bundle vertices placed by the engine's drawn transform, in source axes."""
    signs = np.asarray(AXIS_SIGNS)
    matrix = np.asarray(row['matrix']) * signs[:, None] * signs[None, :]
    position = np.asarray(row['position']) * signs
    points = place(np.asarray(mesh.pos, dtype=np.float64) / float(mesh.position_scale), matrix, position)
    return points if placement is None else source_points(points, placement)


def drawn_ids(ids_dir, bundle_path, source):
    """Source material and textured maps for every captured triangle-id frame."""
    root = pathlib.Path(ids_dir)
    metadata = json.loads((root / 'entries.json').read_text())
    entries, _copies = mesh_bundle.decode(pathlib.Path(bundle_path).read_bytes())
    texture_path = getattr(source, 'texture_pack', None) or metadata.get('texture_pack')
    textures = parse_pack((root / texture_path).read_bytes()) if texture_path else {}
    materials, textured = [], []
    for expected, path in enumerate(sorted(root.glob('*.ids'))):
        frame = int(path.stem)
        if frame != expected:
            raise ValueError('captured frames must start at zero without gaps')
        ids = np.fromfile(path, '<u4').reshape(metadata['height'], metadata['width'])
        material = np.full(ids.shape, EMPTY, dtype=np.int32)
        pattern = np.zeros(ids.shape, dtype=bool)
        covered = ids == 0
        for row in metadata['entries']:
            if row['frame'] != frame:
                continue
            if row['step'] != 'triangle':
                raise ValueError('material attribution requires --view triangles')
            mesh = read_bundle_mesh(entries, row['entry'])
            _near, source_material, _facing = uv_transfer.nearest_materials(entry_points(mesh, row, getattr(source, 'material_placement', None)), mesh.tris, source)
            if row['triangle_count'] != len(source_material) or row['id_count'] != len(source_material):
                raise ValueError('captured triangle count differs from bundle')
            own = (ids >= row['first_id']) & (ids < row['first_id'] + row['id_count'])
            if np.any(covered & own):
                raise ValueError('captured id ranges overlap')
            local = ids[own].astype(np.int64) - row['first_id']
            material[own] = source_material[local]
            uv = textures.get(uv_transfer.name_for_mesh(row['entry']))
            if uv is not None:
                if uv[0] != uv_transfer.TYPE:
                    raise ValueError('texture sibling is not LUVS')
                codes = uv_transfer.decode(uv[1]).codes
                if len(codes) != len(source_material):
                    raise ValueError('texture codes differ from bundle triangles')
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
    result[(ref == EMPTY) != (drawn == EMPTY)] = ONE_EMPTY
    return result


def material_table(references, classes, names):
    """CSV dictionaries by reference material; empty reference pixels have their own row."""
    rows = []
    for material in range(EMPTY, len(names)):
        row = dict(material=material, name=names[material] if material != EMPTY else '(empty)')
        for code, name in enumerate(CLASS_NAMES):
            row[name] = sum(int(np.count_nonzero((ref == material) & (diff == code)))
                            for ref, diff in zip(references, classes))
        rows.append(row)
    return rows
