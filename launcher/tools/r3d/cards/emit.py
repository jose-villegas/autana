"""Build one card shape for the importer's mesh and LMSK writers."""

import numpy as np

from r3d import log
from r3d.cards.bake import bake_card, corner_colours
from r3d.cards.choose import choose_cards
from r3d.mask_asset import UV_FRACTION_BITS
from r3d.light import albedo_from_uv
from r3d.objects.instances import Instance
from r3d.ray_query import RayQuery

QUAD_TRIANGLES = np.array([[0, 1, 2], [2, 1, 3]])
QUAD_CORNERS = np.array([[0, 0], [1, 0], [0, 1], [1, 1]])
UV_SCALE = 1 << UV_FRACTION_BITS


def make_shape(src, material, settings, seed, keep_alpha, *, path=None, occluder=None):
    from r3d.cards.choose import split_cards

    ids = np.flatnonzero(src.tri_m == material)
    triangles, tri_uv = src.tri_v[ids], src.tri_t[ids]
    selected = choose_cards(src.p, triangles, seed=seed, epsilon=settings.epsilon, area_cost=settings.area_cost,
                            path=path, occluder=occluder, uv=src.uv, tri_uv=tri_uv,
                            texture=src.textures[material], keep_alpha=keep_alpha)
    cards, splits = split_cards(selected)
    query = RayQuery(src.p, triangles)
    positions, colours, masks, uvs = [], [], [], []
    rows = []
    for card in cards:
        baked = bake_card(card, src.p, triangles, src.uv, tri_uv, src.textures[material], keep_alpha, query=query)
        if baked is None:
            continue
        card, mask, albedo, coverage = baked
        xy = card['lo'] + QUAD_CORNERS * (card['hi'] - card['lo'])
        positions.append(card['c'] + xy[:, :1] * card['a'] + xy[:, 1:] * card['b'])
        rgb = corner_colours(mask, albedo, coverage)
        kd = src.materials.get(settings.material, {}).get('Kd', (1.0, 1.0, 1.0))
        colours.append(rgb * (np.asarray(kd) if src.textures[material] is not None else
                              albedo_from_uv(None, kd, rgb, None)))
        height, width = mask.shape
        uvs.extend((QUAD_CORNERS * [width * UV_SCALE - 1, height * UV_SCALE - 1] +
                     [0, len(rows) * UV_SCALE]).tolist())
        rows.extend(sum(int(bit) << x for x, bit in enumerate(row)) for row in mask)
        masks.append(card)
    count = len(positions)
    report = dict(candidates=len(selected), split_cards=len(cards), cards=count,
                  triangles=count * len(QUAD_TRIANGLES), splits=splits)
    log(f"cards: {len(selected)} selected, {len(cards)} after splitting, {count} baked, "
        f"{report['triangles']} masked triangles, {splits} splits")
    if not count:
        return None, report
    p = np.concatenate(positions)
    t = np.concatenate([QUAD_TRIANGLES + k * len(QUAD_CORNERS) for k in range(count)])
    labels = np.full(len(t), material)
    normal = np.repeat([card['n'] for card in masks], len(QUAD_CORNERS), axis=0)
    instance = Instance(str(src.tri_o[ids[0]]), np.eye(3), np.zeros(3), False, 0.0)
    shape = dict(positions=p, tris=t, materials=labels, normals=normal, albedo=np.concatenate(colours),
                 mask_uvs=np.asarray(uvs), mask_rows=rows, rule=dict(match=dict(material=settings.material)),
                 copies=[(instance, (p, None, t, labels))])
    return shape, report
