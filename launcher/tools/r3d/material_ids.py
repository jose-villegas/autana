"""Attribute reference and baked pixels to their source and count material differences and exclusions."""
import argparse
from collections import Counter, defaultdict
import hashlib
import json
import math
import pathlib
import shlex

import numpy as np

from asset.asset_pack import parse_pack
from asset.engine_frame import AXIS_COUNT, mirror_matrix, mirror_point
from r3d import mesh_asset, mesh_bundle
from render import reference_poses

EMPTY = -1
CLASS_NAMES = ('both_empty', 'agree_textured', 'agree_plain', 'miss', 'wrong_material', 'reference_only',
               'drawn_only', 'excluded')
BOTH_EMPTY, AGREE_TEXTURED, AGREE_PLAIN, MISS, WRONG_MATERIAL, REFERENCE_ONLY, DRAWN_ONLY, EXCLUDED = range(
    len(CLASS_NAMES))
DISAGREEMENTS = (MISS, WRONG_MATERIAL, REFERENCE_ONLY, DRAWN_ONLY)
EXCLUDE_MATERIAL = '--exclude-material'
EXCLUDE_OBJECT = '--exclude-object'
EXCLUDE = '--exclude'
ALPHA_TESTED = 'alpha-tested'
NOT_DRAWN = 'not-drawn'
THIN = 'thin='
SELECTOR_HELP = f'choose {ALPHA_TESTED}, {NOT_DRAWN} or {THIN}T (finite, nonnegative)'


def selector(value):
    if value in (ALPHA_TESTED, NOT_DRAWN):
        return value
    if value.startswith(THIN):
        try:
            limit = float(value.removeprefix(THIN))
        except ValueError:
            raise argparse.ArgumentTypeError(SELECTOR_HELP) from None
        if math.isfinite(limit) and limit >= 0:
            return value
    raise argparse.ArgumentTypeError(SELECTOR_HELP)


def source_ids(source, query, views, placement=None):
    """Top-first source material and object maps traced through the caller's alpha-tested query."""
    from r3d.fit import gbuffer

    materials, objects = [], []
    owners = np.stack((source.tri_m, source.tri_o))
    for matrix, (width, height) in views:
        if placement is not None:
            axes = AXIS_COUNT
            world = np.eye(axes + 1)
            world[:axes, :axes], world[:axes, axes] = placement.matrix, placement.position
            matrix = matrix @ world
        _depth, shown = gbuffer.traced_buffer(query, matrix, (width, height))
        shown = shown[::-1]
        labels = np.full((len(owners), *shown.shape), EMPTY, dtype=np.int32)
        keep = shown >= 0
        labels[:, keep] = owners[:, shown[keep]]
        material, obj = labels
        materials.append(material)
        objects.append(obj)
    return materials, objects


def entry_points(mesh, row, placement=None):
    """Bundle vertices in source axes and relative to the renderer's placement, if given."""
    from r3d.objects.instances import place

    matrix = mirror_matrix(np.asarray(row['matrix']))
    position = mirror_point(row['position'])
    points = place(np.asarray(mesh.pos, dtype=np.float64) / float(mesh.position_scale), matrix, position)
    if placement is None:
        return points
    inverse = np.linalg.inv(np.asarray(placement.matrix))
    return place(points - placement.position, inverse, np.zeros(AXIS_COUNT))


def bundle_placements(entries, copies, placement):
    """Each bundle mesh as placed for drawing, in the capture row's engine axes."""
    prototypes = {row['mesh'] for row in copies}
    rests = (dict(mesh=name, matrix=np.eye(AXIS_COUNT), position=np.zeros(AXIS_COUNT))
             for name, kind, _ in entries if kind == mesh_asset.TYPE and name not in prototypes)
    for row in (*rests, *copies):
        matrix, position = row['matrix'], row['position']
        if placement is not None:
            matrix = placement.matrix @ matrix
            position = placement.matrix @ position + placement.position
        yield dict(entry=row['mesh'], matrix=mirror_matrix(np.asarray(matrix)), position=mirror_point(position))


def drawn_ids(ids_dir, bundle_path, source, placement=None, texture_pack=None, *, drawn_objects=None):
    """Captured material and texture maps, optionally collecting all source objects mapped by the bundle."""
    from r3d.lit_mesh import read_bundle_mesh
    from r3d.texture import uv_transfer

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

    entries, copies = mesh_bundle.decode(bundle)
    embedded = {name: (kind, blob) for name, kind, blob in entries}
    textures = parse_pack(pack) if pack is not None else {}
    meshes, mappings = {}, {}
    search = uv_transfer.material_search(source)

    def mapped_entry(row):
        name = row['entry']
        if name not in meshes:
            mesh = read_bundle_mesh(entries, name)
            sibling = uv_transfer.name_for_mesh(name)
            uv = textures.get(sibling, embedded.get(sibling))
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
            near, material, *_ = uv_transfer.nearest_materials(points, mesh.tris, source, search)
            mappings[key] = (near if drawn_objects is not None else None), material
        near, material = mappings[key]
        return near, material, codes

    materials, textured = [], []
    for frame, path in paths:
        ids = np.fromfile(path, '<u4').reshape(metadata['height'], metadata['width'])
        material = np.full(ids.shape, EMPTY, dtype=np.int32)
        pattern = np.zeros(ids.shape, dtype=bool)
        next_id = 1
        for row in frames.get(frame, ()):
            if row['step'] != 'triangle':
                raise ValueError('material attribution requires --view triangles')
            _, source_material, codes = mapped_entry(row)
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
        if np.any(ids >= next_id):
            raise ValueError(f'{path}: id outside captured entry ranges')
        materials.append(material)
        textured.append(pattern)
    if drawn_objects is not None:
        owners = np.asarray(source.tri_o)
        for row in bundle_placements(entries, copies, placement):
            near, _, _ = mapped_entry(row)
            drawn_objects.update(np.unique(owners[near]))
    return materials, textured


def exclusion_ids(source, references, objects, selectors, drawn_objects=()):
    """First matching selector index per reference pixel, or EMPTY."""
    from r3d.geometry import bounds_diagonal, grouped_indices, triangle_areas
    from r3d.obj import alpha_tested
    from r3d.texture import uv_transfer

    result = [np.full(ref.shape, EMPTY, dtype=np.int32) for ref in references]
    thickness = None
    for index, (option, value) in enumerate(selectors):
        maps = objects
        if option == EXCLUDE_MATERIAL:
            maps, matches = references, uv_transfer.matched(source.names, [value])
        elif option == EXCLUDE_OBJECT:
            matches = uv_transfer.matched(source.object_names, [value])
        elif value == ALPHA_TESTED:
            maps, matches = references, np.flatnonzero(alpha_tested(source.textures))
        elif value == NOT_DRAWN:
            matches = sorted(set(range(len(source.object_names))) - set(drawn_objects))
        else:
            limit = float(value.removeprefix(THIN))
            if thickness is None:
                thickness = {}
                areas = triangle_areas(source.p, source.tri_v)
                for obj, faces in grouped_indices(source.tri_o).items():
                    size = bounds_diagonal(source.p[np.unique(source.tri_v[faces])])
                    thickness[obj] = areas[faces].sum() / size if size else 0
            matches = [obj for obj, width in thickness.items() if width < limit]
        for ref, labels, first in zip(references, maps, result):
            first[(ref != EMPTY) & (first == EMPTY) & np.isin(labels, matches)] = index
    return result


def material_exclusions(references, objects, exclusions, selectors, source):
    """Excluded pixel counts by first selector, reference object and reference material."""
    counts = Counter()
    for ref, owners, ids in zip(references, objects, exclusions):
        excluded = ids != EMPTY
        keys = np.column_stack([ids[excluded], owners[excluded], ref[excluded]])
        groups, pixels = np.unique(keys, axis=0, return_counts=True)
        counts.update({tuple(group): int(count) for group, count in zip(groups, pixels)})
    return [dict(selector=shlex.join(selectors[index]), object=int(obj), name=source.object_names[obj],
                 material=source.names[material], pixels=count)
            for (index, obj, material), count in sorted(counts.items(), key=lambda row: (-row[1], row[0]))]


def diff_classes(ref, drawn, textured, has_texture, exclusions=None):
    """One mutually exclusive class per pixel, including both kinds of empty pixel."""
    ref, drawn, textured = np.asarray(ref), np.asarray(drawn), np.asarray(textured)
    if ref.shape != drawn.shape or ref.shape != textured.shape:
        raise ValueError('material maps differ in size')
    result = np.full(ref.shape, BOTH_EMPTY, dtype=np.uint8)
    occupied = (ref != EMPTY) & (drawn != EMPTY)
    agree = occupied & (ref == drawn)
    result[agree & textured] = AGREE_TEXTURED
    plain = agree & ~textured
    result[plain] = np.where(np.asarray(has_texture, dtype=bool)[ref[plain]], MISS, AGREE_PLAIN)
    result[occupied & ~agree] = WRONG_MATERIAL
    result[(ref != EMPTY) & (drawn == EMPTY)] = REFERENCE_ONLY
    result[(ref == EMPTY) & (drawn != EMPTY)] = DRAWN_ONLY
    if exclusions is not None:
        result[(ref != EMPTY) & (exclusions != EMPTY)] = EXCLUDED
    return result


def material_table(references, drawn, classes, names):
    """CSV counts by reference material, with drawn-only pixels under their drawn material."""
    counts = np.zeros((len(names) + 1, len(CLASS_NAMES)), dtype=np.int64)
    for ref, own, diff in zip(references, drawn, classes):
        owner = np.where(ref != EMPTY, ref, own)
        keys = (owner - EMPTY) * len(CLASS_NAMES) + diff
        counts.flat[:] += np.bincount(keys.ravel(), minlength=counts.size)
    return [dict(material=material, name=name, **dict(zip(CLASS_NAMES, totals)))
            for material, (name, totals) in enumerate(zip(['(empty)', *names], counts.tolist()), start=EMPTY)]


def material_confusions(references, drawn, classes):
    """Wrong-material counts by source material pair, sorted by decreasing pixel count."""
    counts = Counter()
    for ref, own, diff in zip(references, drawn, classes):
        wrong = diff == WRONG_MATERIAL
        pairs, pixels = np.unique(np.column_stack([ref[wrong], own[wrong]]), axis=0, return_counts=True)
        counts.update({tuple(pair): int(count) for pair, count in zip(pairs, pixels)})
    return [dict(reference_material=int(pair[0]), drawn_material=int(pair[1]), pixels=count)
            for pair, count in sorted(counts.items(), key=lambda row: (-row[1], row[0]))]
