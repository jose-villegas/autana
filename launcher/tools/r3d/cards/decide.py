"""Select cut-out materials by their alpha-cut surface samples and their baked cards' opaque-area coverage."""

import numpy as np

from r3d.cards import CARD_COVERED_MIN, CARD_SEE_THROUGH_MIN
from r3d.import_settings import SettingsError, match_meshes
from r3d.cards.choose import material_samples
from r3d.obj import alpha_tested


def see_through_share(src, ids, keep_alpha, seed):
    """The share of surface samples on triangles `ids` that the alpha test cuts: leaves cut tight around each
    triangle count as see-through, though no triangle's mean alpha is under keep_alpha."""
    _, _, (_, _, _, opaque) = material_samples(src, ids, keep_alpha, seed)
    return float(1 - opaque.mean()) if len(opaque) else 0.0


def candidates(src, settings):
    """(material, see-through share) for each material the cards may replace: one with a cut-out texture, not
    carved by an objects.meshes rule, see-through enough (`see_through_share`); or named by `always`, its share
    None."""
    cards = settings.cards
    unknown = sorted((set(cards.never) | set(cards.always)) - set(src.names))
    if unknown:
        raise SettingsError(f"geometry.cards names materials the source lacks: {', '.join(unknown)}")
    rules = match_meshes(src.tri_o, src.tri_m, src.object_names, src.names, settings.meshes) if settings.meshes else {}
    tested = alpha_tested(src.textures)
    out = []
    for material, name in enumerate(src.names):
        ids = np.flatnonzero(src.tri_m == material)
        if name in cards.never or not len(ids):
            continue
        drawn_apart = any(obj in rules for obj in src.tri_o[ids])
        if name in cards.always:
            if drawn_apart:
                raise SettingsError(f"material {name} is carved by an objects.meshes rule")
            out.append((material, None))
        elif not drawn_apart and tested[material]:
            share = see_through_share(src, ids, settings.alpha_keep, settings.seed)
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
