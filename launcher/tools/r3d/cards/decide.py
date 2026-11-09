"""Which materials cards replace, decided from the source with no per-scene list.

A material with a cut-out texture is a candidate when enough of its triangles are mostly see-through
(CARD_SEE_THROUGH_MIN); its cards are made, and kept only when they cover enough of its opaque surface
(CARD_COVERED_MIN). Every other material, a rejected candidate included, stays fitted geometry. The import's
`geometry.cards.never` and `always` override the decision for the materials they name.
"""

import numpy as np

from r3d.cards import CARD_COVERED_MIN, CARD_SEE_THROUGH_MIN
from r3d.import_settings import SettingsError, match_meshes
from r3d.light import has_alpha, triangle_alpha


def see_through_share(src, ids, texture, keep_alpha):
    """The share of triangles `ids` whose mean texture alpha is under keep_alpha."""
    return float(np.mean(triangle_alpha(src.uv, src.tri_t[ids], texture) < keep_alpha))


def candidates(src, settings):
    """(material, see-through share) for each material the cards may replace: one with a cut-out texture, not
    carved by an objects.meshes rule nor thinned, see-through enough; or named by `always`, its share None."""
    cards = settings.cards
    unknown = sorted((set(cards.never) | set(cards.always)) - set(src.names))
    if unknown:
        raise SettingsError(f"geometry.cards names materials the source lacks: {', '.join(unknown)}")
    rules = match_meshes(src.tri_o, src.tri_m, src.object_names, src.names, settings.meshes) if settings.meshes else {}
    thin = settings.thin.material if settings.thin else None
    out = []
    for material, name in enumerate(src.names):
        ids = np.flatnonzero(src.tri_m == material)
        if name in cards.never or not len(ids):
            continue
        drawn_apart = name == thin or any(obj in rules for obj in src.tri_o[ids])
        if name in cards.always:
            if drawn_apart:
                raise SettingsError(f"material {name} is drawn by cards")
            out.append((material, None))
        elif not drawn_apart and has_alpha(src.textures[material]):
            share = see_through_share(src, ids, src.textures[material], settings.alpha_keep)
            if share >= CARD_SEE_THROUGH_MIN:
                out.append((material, share))
    return out


def carded(src, settings, make):
    """[(material, shape, report)] for the materials cards replace, and every candidate's report keyed by its
    name; `make(material)` builds a candidate's (shape, report) (cards.emit.make_shape)."""
    accepted, reports = [], {}
    for material, share in candidates(src, settings):
        shape, report = make(material)
        forced = share is None
        report = dict(report, see_through=share, forced=forced,
                      accepted=shape is not None and (forced or report['covered'] >= CARD_COVERED_MIN))
        reports[src.names[material]] = report
        if report['accepted']:
            accepted.append((material, shape, report))
    return accepted, reports
