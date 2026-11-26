"""Re-measure a simplified prototype's surface error."""

import numpy as np

from r3d.geometry import bounds_diagonal, nearest_triangles, triangle_search
from r3d.meshopt import simplify_to_error

MAX_ERROR_SHRINK = 0.5


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
        asked *= min(error / moved, MAX_ERROR_SHRINK)
        asked = asked if asked >= floor else 0.0


def surface_error(source_positions, source_triangles, positions, triangles):
    """The largest distance from either surface's vertices to the other surface, in model units."""
    if not len(triangles):
        return bounds_diagonal(source_positions)
    there = nearest_triangles(source_positions[np.unique(source_triangles)], triangle_search(positions[triangles]))[2]
    back = nearest_triangles(positions[np.unique(triangles)], triangle_search(source_positions[source_triangles]))[2]
    return float(max(there.max(initial=0.0), back.max(initial=0.0)))
