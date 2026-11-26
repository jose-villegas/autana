"""Build a card shape with vertex light, page UVs and MASK1 textures."""

import numpy as np

from r3d import log
from r3d.cards.bake import bake_card, corner_colours
from r3d.cards.choose import choose_cards, material_samples, probe_weights, split_cards
from r3d.cards import decide
from r3d.cards.pages import PAGE_SIDE
from r3d.mesh_source import with_triangles
from r3d.obj import Texture
from r3d.poses import camera_rays
from r3d.reference_set import PROBE_SEED
from r3d.views import probes
from r3d.cards import CARD_MIP_LEVELS, PROBE_WEIGHT_FLOOR
from r3d.cards.pages import pack_pages
from r3d.texture.texture_asset import mask_texture, pack_ltex
from r3d.texture.uv_transfer import UV_SHIFT
from r3d.light import AlphaTest, albedo_from_uv
from r3d.ray_query import RayQuery

QUAD_TRIANGLES = np.array([[0, 1, 2], [2, 1, 3]])
QUAD_CORNERS = np.array([[0, 0], [1, 0], [0, 1], [1, 1]])
UV_SCALE = 1 << UV_SHIFT
SIDE_PERCENTILES = dict(min=0, median=50, p90=90, max=100)


def share(weight, chosen):
    """`chosen`'s share of the total `weight`, 0 when there is none."""
    total = weight.sum()
    return float(weight[chosen].sum() / total) if total else 0.0


def make_shape(src, material, settings, seed, keep_alpha, *, views, query):
    name = src.names[material]
    ids = np.flatnonzero(src.tri_m == material)
    sub, rng, (points, normals, area, opaque) = material_samples(src, ids, keep_alpha, seed)
    sampled = points, normals, area = points[opaque], normals[opaque], area[opaque]
    weights = probe_weights(points, views.training, query, views.face_lens,
                            near_floor=PROBE_WEIGHT_FLOOR * views.record['spacing'])
    selected = choose_cards(sampled, rng, weights=weights, epsilon=settings.epsilon, area_cost=settings.area_cost)
    long_sides = np.array([max(card['hi'] - card['lo']) for card in selected])
    distribution = (dict(zip(SIDE_PERCENTILES, np.percentile(long_sides, list(SIDE_PERCENTILES.values())).tolist()))
                    if len(selected) else {})
    log("cards: long side before splitting (model units): " +
        ", ".join(f"{key}={value:.3f}" for key, value in distribution.items()))
    cards, splits = split_cards(selected)
    query_source = query
    query = RayQuery(sub.p, sub.tri_v, hit_filter=AlphaTest(sub, keep_alpha))
    positions, colours, masks, planes = [], [], [], []
    represented = np.zeros(len(points), bool)
    for card in cards:
        baked = bake_card(card, query)
        if baked is None:
            continue
        card, mask, albedo, coverage = baked
        relative = points - card['c']
        depth = relative @ card['n']
        projected = np.column_stack([relative @ card['a'], relative @ card['b']])
        inside = ((depth >= card['depth'][0]) &
                  (depth <= card['depth'][1]) &
                  (projected >= card['lo']).all(axis=1) & (projected <= card['hi']).all(axis=1))
        texels = ((projected[inside] - card['lo']) / (card['hi'] - card['lo']) *
                  np.array(mask.shape[::-1])).astype(int)
        texels = np.minimum(texels, np.array(mask.shape[::-1]) - 1)
        represented[inside] |= mask[texels[:, 1], texels[:, 0]]
        xy = card['lo'] + QUAD_CORNERS * (card['hi'] - card['lo'])
        positions.append(card['c'] + xy[:, :1] * card['a'] + xy[:, 1:] * card['b'])
        rgb = corner_colours(mask, albedo, coverage)
        kd = src.materials.get(name, {}).get('Kd', (1.0, 1.0, 1.0))
        colours.append(rgb * (np.asarray(kd) if src.textures[material] is not None else
                              albedo_from_uv(None, kd, rgb, None)))
        masks.append(mask)
        planes.append(card)
    pages, rects = pack_pages(masks)
    textures = [mask_texture(page, CARD_MIP_LEVELS) for page in pages]
    # Keep the max corner inside the last texel: the next UV unit samples the empty page border.
    uvs = np.concatenate([QUAD_CORNERS * [width * UV_SCALE - 1, height * UV_SCALE - 1] +
                          [x * UV_SCALE, y * UV_SCALE] for page, x, y, width, height in rects]) if rects else []
    count = len(positions)
    report = dict(candidates=len(selected), split_cards=len(cards), cards=count,
                  triangles=count * len(QUAD_TRIANGLES), splits=splits, long_sides=distribution,
                  covered=share(area * weights, represented), covered_area=share(area, represented),
                  pages=len(pages), texture_bytes=sum(len(pack_ltex(texture)) for texture in textures),
                  texel_size=cards[0]['texel_size'] if cards else None, probes=views.record)
    log(f"cards: {len(selected)} selected, {len(cards)} after splitting, {count} baked, "
        f"{report['triangles']} triangles, {splits} splits, "
        f"{len(pages)} MASK1 pages, {report['texture_bytes']} texture bytes")
    if not count:
        return None, report
    p = np.concatenate(positions)
    t = np.concatenate([QUAD_TRIANGLES + k * len(QUAD_CORNERS) for k in range(count)])
    labels = np.full(len(t), material)
    normal = np.repeat([card['n'] for card in planes], len(QUAD_CORNERS), axis=0)
    shape = dict(positions=p, tris=t, materials=labels, normals=normal, albedo=np.concatenate(colours),
                 vertex_uvs=np.asarray(uvs), pages=textures, page_of_card=[r[0] for r in rects],
                 name=name, rule=dict(match=dict(material=name)),
                 owner=str(src.tri_o[ids[0]]), labels=labels)
    report["held_out"] = score_views(src, material, shape, query_source, views.held_out, keep_alpha, views.face_lens)
    return shape, report


def carded(src, settings, scene):
    """Accepted (material, shape, report) rows and reports, sharing the full-source alpha query."""
    if settings.cards is None:
        return [], {}
    query = RayQuery(src.p, src.tri_v, hit_filter=AlphaTest(src, settings.alpha_keep))

    def make(material):
        views, _ = material_views(src, material, settings, scene, query=query)
        return make_shape(src, material, settings.cards, settings.seed, settings.alpha_keep,
                          views=views, query=query)

    return decide.carded(src, settings, make)


def material_views(src, material, settings, scene, *, query=None):
    """Target one material through the unmasked alpha-tested source, using the shared probe selection."""
    camera = getattr(getattr(scene, 'camera', None), 'component', None)
    region = None if camera is None else camera.region
    near = None if camera is None else camera.near_z
    options = settings.fit.probes
    spacing = None if options is None else options.spacing
    query = (RayQuery(src.p, src.tri_v, hit_filter=AlphaTest(src, settings.alpha_keep))
             if query is None else query)
    views = probes.select(src.p, src.tri_v, region, near, spacing, PROBE_SEED, query=query,
                          targets=src.tri_m == material)
    return views, query


def score_views(src, material, shape, query, faces, keep_alpha, face_lens):
    """Held-out silhouette IoU, precision and recall, with other source materials still occluding the cards."""
    if not faces:
        return None
    textures = list(src.textures)
    for page in shape['pages']:
        rgba = np.ones((PAGE_SIDE, PAGE_SIDE, 4))
        rgba[..., 3] = page.levels[0]
        textures.append(Texture.from_levels([rgba]))
    tris = np.concatenate([QUAD_TRIANGLES + k * len(QUAD_CORNERS) for k in range(len(shape['page_of_card']))])
    other = src.tri_m != material
    uvs = shape['vertex_uvs'] / (UV_SCALE * PAGE_SIDE)
    uvs = np.column_stack([uvs[:, 0], 1 - uvs[:, 1]])
    candidate = with_triangles(src, p=np.concatenate([src.p, shape['positions']]),
                                uv=np.concatenate([src.uv, uvs]), textures=textures,
                                tri_v=np.concatenate([src.tri_v[other], tris + len(src.p)]),
                                tri_t=np.concatenate([src.tri_t[other], tris + len(src.uv)]),
                                tri_m=np.concatenate([src.tri_m[other],
                                                      np.repeat(shape['page_of_card'], len(QUAD_TRIANGLES)) +
                                                      len(src.textures)]))
    predicted = RayQuery(candidate.p, candidate.tri_v, hit_filter=AlphaTest(candidate, keep_alpha))
    intersection = union = expected_count = predicted_count = 0
    for pose in faces:
        origins, directions = camera_rays(probes.SELECT_SIZE, probes.SELECT_SIZE, face_lens, pose[:3], pose[3:], 1)
        hit, _, tri = query.first_hits(origins, directions)
        expected = hit & (src.tri_m[np.where(hit, tri, 0)] == material)
        hit, _, tri = predicted.first_hits(origins, directions)
        actual = hit & (candidate.tri_m[np.where(hit, tri, 0)] >= len(src.textures))
        intersection += int(np.count_nonzero(expected & actual))
        union += int(np.count_nonzero(expected | actual))
        expected_count += int(np.count_nonzero(expected))
        predicted_count += int(np.count_nonzero(actual))
    return dict(faces=len(faces), intersection=intersection, union=union,
                expected=expected_count, predicted=predicted_count,
                iou=intersection / union if union else None,
                precision=intersection / predicted_count if predicted_count else None,
                recall=intersection / expected_count if expected_count else None)
