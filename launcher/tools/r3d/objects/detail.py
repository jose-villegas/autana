"""How much detail a carved shape keeps, derived from the views and the runtime's one LOD threshold.

A shape's near level has just enough triangles that, seen from the closest the camera comes to any of its copies,
no surface moves more than lod.tolerance_px on screen. Its far levels are the levels stage's, picked at run time
against the same threshold. When the scene's ceiling binds, balance() steps shapes down a ladder of coarser
rungs, the one that removes the most triangles per pixel of error first. needs_authoring() names every shape
whose kept level still errs above the threshold up close, with the reason.
"""

from dataclasses import dataclass

import numpy as np

from r3d.geometry import bounds_diagonal, nearest_triangles, triangle_search
from r3d.meshopt import simplify_to_error
from render.settings import render_asset

# The screen error the level pick allows, in pixels: the render asset's, as r3d.lod.pick reads it.
TOLERANCE_PX = render_asset.load(render_asset.DEFAULT)["lod.tolerance_px"]

# Each rung of a shape's ladder allows this many times the screen error of the rung before.
LADDER_STEP = 2.0


@dataclass(frozen=True)
class Rung:
    triangles: int
    error_px: float  # the screen error this rung allows at the shape's nearest view


@dataclass(frozen=True)
class Authoring:
    shape: str
    error_px: float  # the kept level's largest surface move at the shape's nearest view
    reason: str  # 'ceiling': the balance stepped it past the threshold; 'silhouette': its stand-in's surface moved


def nearest_distance(lows, highs, eyes, near):
    """The closest any of `eyes` comes to any copy's bounds (lows, highs per copy), never nearer than the camera's
    near plane, since nothing nearer is drawn. With no eyes the camera may stand anywhere: the near plane."""
    eyes = np.asarray(eyes, dtype=np.float64).reshape(-1, 3)
    if not len(eyes):
        return float(near)
    lows, highs = np.asarray(lows, dtype=np.float64), np.asarray(highs, dtype=np.float64)
    gap = np.maximum(np.maximum(lows[None] - eyes[:, None], eyes[:, None] - highs[None]), 0.0)
    return max(float(np.linalg.norm(gap, axis=2).min()), float(near))


def object_error(distance, focal, error_px=TOLERANCE_PX):
    """The model-space move that projects to `error_px` at `distance` through `focal` pixels per unit depth."""
    return error_px * distance / focal


def level(positions, triangles, error):
    """The fewest triangles meshoptimizer keeps whose surface, measured, moves at most `error`: its own error is an
    estimate, so a level that measures over is simplified again at the error scaled by how far over it was. Once
    that error falls below the positions' float32 resolution it is 0; meshoptimizer still drops some parts at 0
    (a zero-area sliver), and when even that level measures over, the shape is kept whole."""
    asked, floor = error, bounds_diagonal(positions) * np.finfo(np.float32).eps
    while True:
        kept = simplify_to_error(positions, triangles, asked)
        if len(kept) == len(triangles):
            return kept
        moved = surface_error(positions, triangles, positions, kept)
        if moved <= error:
            return kept
        if asked == 0.0:
            return triangles
        asked *= error / moved
        asked = asked if asked >= floor else 0.0


def ladder(positions, triangles, distance, focal, tolerance_px=TOLERANCE_PX):
    """The shape's rungs: the near level at tolerance_px, then each LADDER_STEP times coarser, while the triangle
    count still drops and the allowed move stays inside the shape."""
    rungs = [Rung(len(level(positions, triangles, object_error(distance, focal, tolerance_px))), tolerance_px)]
    error_px = tolerance_px * LADDER_STEP
    while object_error(distance, focal, error_px) < bounds_diagonal(positions):
        count = len(level(positions, triangles, object_error(distance, focal, error_px)))
        if count < rungs[-1].triangles:
            rungs.append(Rung(count, error_px))
        error_px *= LADDER_STEP
    return rungs


def balance(ladders, room):
    """Each shape's rung: the first, unless their triangles exceed `room` (None: no cap); then, one step at a time,
    the shape whose next rung removes the most triangles per pixel of error added. Stops at the last rungs."""
    picks = [0] * len(ladders)
    total = sum(rungs[0].triangles for rungs in ladders)
    while room is not None and total > room:
        steps = [((rungs[pick].triangles - rungs[pick + 1].triangles) / (rungs[pick + 1].error_px - rungs[pick].error_px),
                  -index) for index, (rungs, pick) in enumerate(zip(ladders, picks)) if pick + 1 < len(rungs)]
        if not steps:
            break
        index = -max(steps)[1]
        total -= ladders[index][picks[index]].triangles - ladders[index][picks[index] + 1].triangles
        picks[index] += 1
    return picks


def surface_error(source_positions, source_triangles, positions, triangles):
    """The largest distance from either surface's vertices to the other surface, in model units."""
    if not len(triangles):
        return bounds_diagonal(source_positions)
    there = nearest_triangles(source_positions[np.unique(source_triangles)], triangle_search(positions[triangles]))[2]
    back = nearest_triangles(positions[np.unique(triangles)], triangle_search(source_positions[source_triangles]))[2]
    return float(max(there.max(initial=0.0), back.max(initial=0.0)))


def needs_authoring(rows, tolerance_px=TOLERANCE_PX):
    """The shapes whose kept level errs above `tolerance_px` at their nearest view. Each row is (name, error_px,
    stepped): `stepped` when the balance took the shape past its near level."""
    return [Authoring(name, error_px, 'ceiling' if stepped else 'silhouette')
            for name, error_px, stepped in rows if error_px > tolerance_px]
